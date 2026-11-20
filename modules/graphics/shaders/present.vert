#version 300 es

/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The full-screen present pass: the first thing any hardware-accelerated path needs, and the seed this
// directory was created for. It draws a quad in clip space and hands the texture coordinate to the
// fragment stage.
//
// Nothing loads it yet. It exists so that `tools/shader_compiler` and the build rule in
// `shaders/CMakeLists.txt` are exercised by a real shader rather than by an empty directory - a build
// step that compiles nothing is a build step that cannot be seen to be broken.
//
// The conventions, which every shader here follows:
//
//   * `#version` is the **first line**, before this comment and before the licence header. glslang
//     rejects an ES-profile shader whose directive is not the very first thing:
//
//         #version: statement must appear first in es-profile shader; before comments or newlines
//
//     That is why the header sits below it, which is unusual for this tree and is not an oversight.
//   * `#version 300 es`. The compiler rewrites this to 310 internally, because glslang cannot emit
//     SPIR-V from anything below GLSL ES 3.10, and reports that it did. ESSL 3.00 is a strict subset of
//     3.10, so the rewrite is faithful; `shader_compiler` prints a note whenever it happens.
//   * `layout(location = N)` on stage inputs and outputs. SPIR-V requires explicit locations, and a
//     shader without them does not compile to it.
//   * `layout(set = N, binding = M)` on resources. The backend's reflection reads those decorations,
//     which is what lets a caller bind by (set, binding) and never type a uniform name.
//   * A `precision` statement, because GLSL ES has no default for floats.

precision highp float;

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;

void main() {
  vTexCoord = aTexCoord;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
