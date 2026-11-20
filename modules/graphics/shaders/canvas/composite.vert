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

// The vertex half of the full-target passes: `composite.frag`, which composites a saved layer onto
// the target, and `mask.frag`, which multiplies a layer by a mask. Both draw **one quad covering the
// whole attachment** and let the fragment stage decide what each texel becomes.
//
// ## Why there is no `mask.vert`
//
// A vertex stage whose entire job is "cover the attachment and hand the fragment stage a texture
// coordinate" is one program, not three. A copy per fragment shader would be three places for the Y
// convention below to drift apart, and the drift would show as one of the three passes sampling its
// texture mirrored relative to the other two - which, for a composite, means blending each pixel with
// its reflection and looks like a smear rather than like a flip.
//
// ## Why it looks like `present.vert`
//
// Because it is the same operation, and it is written the same way for the same reason: positions
// arrive already in clip space and a texture coordinate is handed straight through. It is not
// `present.vert` itself because the two are not the same pass - `present.vert` belongs to the swap
// chain's final blit and is the concatenation of the conventions this directory establishes, while
// this belongs to the offscreen composites that happen before it. Coupling them would mean a change
// to the present pass re-baking the composite pass, and the two are expected to diverge: a present
// pass gains a colour-space conversion and a composite does not.
//
// ## Which way Y runs
//
// Positions are in clip space, so the Y convention is the backend's rather than this file's: the same
// convention `present.vert` relies on. The `aTexCoord` that goes with each corner is supplied by the
// caller and is *not* derived from the position, precisely so that the caller can hand this stage a
// flipped texture coordinate without the position knowing. The one requirement is that
// `composite.frag`'s two textures and `mask.frag`'s two texture coordinate spaces agree with each
// other, which is the fragment shaders' business, not this one's.
//
// The conventions this directory follows are in `shaders/README.md`: `#version` first, ahead of the
// licence header; `layout(location = N)` on stage inputs and outputs; `layout(set = N, binding = M)`
// on resources; a `precision` statement.

precision highp float;

// Clip-space position. Two vec2 corners and a triangle list, as in `present.vert`: the quad is the
// same six-vertex shape, and matching it means a caller can use one full-target vertex buffer for the
// present pass and the composite passes alike.
layout(location = 0) in vec2 aPosition;

// The texture coordinate for this corner. A separate attribute rather than a reinterpretation of
// `aPosition * 0.5 + 0.5`, because that would only be right for an unflipped, full-extent quad and
// would silently be wrong for the flipped viewport the OpenGL backend uses.
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;

void main() {
  // Passed through unchanged, so the fragment stage samples exactly the texel the caller associated
  // with this corner. Interpolated across the quad by the rasterizer, which is why the texture
  // coordinate has to be a varying at all rather than a constant: a full-target quad needs its
  // coordinates spread across the attachment.
  vTexCoord = aTexCoord;

  // Straight to clip space; no depth, no divide, and no viewport arithmetic to fold in. The quad's
  // own extent is what makes it cover the attachment, so this stage never has to know the target's
  // size - which is why the composite and mask passes take no `uViewport` and the device-space passes
  // do.
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
