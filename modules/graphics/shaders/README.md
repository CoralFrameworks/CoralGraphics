# Shaders

The GLSL the RHI's hardware-accelerated paths are built from.

These are compiled to SPIR-V at build time by [`tools/shader_compiler`](../tools/shader_compiler) and
shipped as `.spv`. Nothing here reaches a device as source.

## Why they are compiled ahead of time

The RHI's canonical shader input is **SPIR-V**: Vulkan consumes it verbatim, and the OpenGL backend
translates it — to `#version 300 es` on mobile and `#version 410 core` on desktop — through SPIRV-Cross.

The RHI can also accept GLSL source at run time, which is what makes a shader debuggable in place. Doing
that costs a linked copy of shaderc, and shaderc brings glslang and SPIRV-Tools with it. Measured as
static archives in a Release Android build:

| library | archive |
| --- | --- |
| SPIRV-Cross | 25.6 MB |
| glslang | 63.9 MB |
| SPIRV-Tools + SPIRV-Tools-opt | 234.5 MB |
| shaderc + shaderc_util | 3.1 MB |

The linker discards most of what is unreferenced, so those are not APK figures — but the ratio is the
point. A build that compiles its own shaders needs only the top row.

## Conventions

Every shader here follows these. `present.vert` is the worked example.

### `#version` must be the first line

Before the licence header, before any comment, before any blank line. This is not a style rule; glslang
enforces it for the ES profile:

```
#version: statement must appear first in es-profile shader; before comments or newlines
```

That is why the Apache header sits *below* the directive in these files, which is unusual for this tree
and is not an oversight.

### `#version 300 es`

The compiler rewrites it to `310` internally, because glslang cannot emit SPIR-V below GLSL ES 3.10, and
reports that it did:

```
note: present.vert declares 300 and was compiled as 310 (ESSL 3.00)
```

ESSL 3.00 is a strict subset of 3.10, so the rewrite is faithful. The note is printed rather than
inferred because "the shader compiled" and "the shader compiled as the language it declared" are
different statements.

### `layout(location = N)` on stage inputs and outputs

SPIR-V requires explicit locations. A shader without them does not compile to it.

### `layout(set = N, binding = M)` on resources

`set` and `binding` are what the backend's reflection reads, which is what lets a caller bind by
`(set, binding)` and never type a uniform name:

```glsl
layout(set = 0, binding = 0) uniform sampler2D uTexture;
```

The OpenGL translation drops `set` — OpenGL has one namespace — and keeps the binding. The pair is
declared anyway because a Vulkan backend consumes the same SPIR-V and needs both.

### A `precision` statement

GLSL ES has no default precision for floats.

## What cannot go through this pipeline

**ESSL 1.00 that uses the keywords ES 3.00 removed.** `attribute`, `varying`, `texture2D` and
`gl_FragColor` were removed in ES 3.00, and glslang cannot emit SPIR-V below GLSL ES 3.10 — so the
retarget that rescues an ESSL 3.00 source cannot rescue these. The compiler says so with its own
diagnostics plus the reason:

```
vertex.glsl:2: error: 'attribute' : no longer supported in es profile; removed in version 300
...
--- this ESSL 1.00 source cannot become SPIR-V ---
'attribute' was removed in OpenGL ES 3.00, and glslang cannot emit SPIR-V below GLSL ES 3.10.
Rewrite it as ES 3.00 (in/out, texture(), an explicit out variable) and declare
#version 300 es, or keep the shader as ESSL 1.00 and do not compile it to SPIR-V.
```

The rewrite is mechanical: `attribute` → `layout(location = N) in`, `varying` → `in`/`out`,
`texture2D(s, uv)` → `texture(s, uv)`, and `gl_FragColor` → a declared `layout(location = 0) out vec4`.

**`.glsl` files.** A file named `thing.glsl` carries no stage in its name, so the build does not know
whether to compile it as a vertex or fragment shader. It is **not** compiled, and the configure says so:

```
-- shaders: 2 .glsl file(s) are NOT compiled, because a .glsl carries no stage in its name.
```

Rename to `.vert`/`.frag`/`.comp`/`.geom`/`.tesc`/`.tese`, or compile by hand with
`shader_compiler <file> --stage <stage>`. Inferring the stage from the file name — `vertex.glsl` is a
vertex shader — is exactly the kind of guess this pipeline avoids: a wrong guess compiles a shader as
the wrong stage, and that surfaces as a link error on a device rather than here.

## Compiling

The compiler is a native executable, so it has to be built for the machine doing the building.

### On the host

A normal host configure builds it and compiles the shaders as part of `ALL`:

```shell
cmake -S . -B build/host -G Ninja
cmake --build build/host --target shaders
```

The `.spv` land in `build/host/shaders/`, mirroring this directory's structure, with `.spv` appended to
the whole name — `present.vert` becomes `present.vert.spv`. The stage stays in the file name on purpose:
replacing the extension instead would make `present.vert` and `present.frag` both write `present.spv`.

### For Android

`add_executable` under the Android toolchain produces a binary the build machine cannot run, so `tools/`
is host-only and the Android configure is given the host tool's path:

```shell
cmake -S . -B build/host -G Ninja
cmake --build build/host --target shader_compiler

cmake -S . -B build/android -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=24 \
      -DCORAL_SHADERS_COMPILER=$PWD/build/host/tools/shader_compiler/shader_compiler
```

Without `CORAL_SHADERS_COMPILER` the configure prints a status line and compiles nothing. That is
deliberate: a developer iterating on the RHI should not be blocked by a tool they are not using.

The Gradle build passes both the compiler path and the output directory — see
`android/coral-android/build.gradle.kts`, and `coral.shaderCompiler` in `gradle.properties`. The `.spv`
are packaged into the APK's `assets/`.

### By hand

```shell
shader_compiler present.vert present.frag                # beside each input, as *.spv
shader_compiler --source-dir . --output-dir out          # a whole tree, mirrored
shader_compiler --source-dir . --output-dir out --verify
```

`--verify` is worth using when adding a shader. It runs the result through the same SPIRV-Cross pass the
OpenGL backend runs and prints the emitted GLSL version and the reflected bindings:

```
  verified: "#version 300 es", 3 binding(s)
    set 0 binding 0  TextureSampler     uTexture
    set 0 binding 0  StageInput         vTexCoord
    set 0 binding 0  StageOutput        oColor
```

A module can be valid SPIR-V and still fail to translate — an unsupported capability, a stage
SPIRV-Cross has no path for. Compiling ahead of time is precisely so that is found here rather than on a
device.

## What the compiler does not do

**No `#include`.** The runtime's compiler has no includer, so supporting one here would make a shader
that this tool accepts and the RHI rejects — a difference that would surface as "it works in the build
and fails on device". If includes are wanted, they belong in the runtime's compiler so both paths gain
them at once.

**No source rewriting beyond the `#version` retarget.** That retarget is the compiler's, not this
directory's, and it happens only when the source declares a version glslang cannot emit SPIR-V from.
A shader is otherwise compiled as written.
