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

// The fragment half of the present pass: sample the source image at the coordinate the vertex stage
// interpolated. See `present.vert` for the conventions this directory follows - in particular that
// `#version` has to be the first line, ahead of the licence header.

precision mediump float;

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 oColor;

// `set` and `binding` are what the backend's reflection reads. The OpenGL translation drops `set` -
// OpenGL has one namespace - and keeps the binding, which is why the pair is declared even though only
// the GL backend exists today: a Vulkan backend consumes the same SPIR-V and needs both.
layout(set = 0, binding = 0) uniform sampler2D uTexture;

void main() {
  oColor = texture(uTexture, vTexCoord);
}
