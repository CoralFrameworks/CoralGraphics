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

/*
 * GENERATED FILE - DO NOT EDIT.
 *
 * Produced by modules/graphics/tools/canvas_glsl_generator from the GLSL under shaders/canvas/.
 *
 * This is the OpenGL backend's canvas shader set for builds without SPIRV-Cross: the
 * sources are the same shaders compiled to SPIR-V for the OpenGL environment and then
 * translated to each driver dialect, exactly as the runtime translator would have done.
 * Each entry carries the binding names the GL backend resolves with
 * `glGetUniformLocation`/`glGetUniformBlockIndex`, because a direct module has no
 * reflection to read them from.
 */

#ifndef CORAL_GPU_CANVAS_SHADERS_GLSL_H
#define CORAL_GPU_CANVAS_SHADERS_GLSL_H

#include <backend/Descriptors.h>

#include <cstddef>
#include <cstdint>

/** The same source stamp as the committed SPIR-V copy; a stale fallback fails a test. */
#ifndef CORAL_CANVAS_GLSL_SHADERS_SOURCE_MANIFEST
#define CORAL_CANVAS_GLSL_SHADERS_SOURCE_MANIFEST "composite.frag:27293:1785283876;composite.vert:4371:850932064;coverage.frag:8687:3324524355;gradient.frag:17131:3906668714;image.frag:7156:2149229792;image_cubic.frag:4142:672872816;mask.frag:8545:3195844262;path.frag:5408:1305144248;path.vert:6016:1586480104;present.frag:5141:1060897216;quad.frag:13907:4111692756;quad.vert:14876:1113030890;sdf.frag:4186:704651425;sdf_pattern.frag:4388:774748181"
#endif

namespace graphics {
namespace gpu {
namespace shaders {
namespace glsl {

/** One shader, in the two dialects the OpenGL backend can come up in. */
struct Source {
  backend::ShaderStage stage = backend::ShaderStage::Vertex;
  const char* name = nullptr;
  const char* es = nullptr;
  const char* desktop = nullptr;
  const backend::ShaderBindingDeclaration* bindings = nullptr;
  size_t bindingCount = 0;

  [[nodiscard]] bool isValid() const noexcept { return es != nullptr && desktop != nullptr; }
};

// composite.frag: 7824 ES bytes, 7391 desktop bytes.
namespace composite_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 2, backend::BindingType::UniformBuffer, 1, "CompositeUniforms" },
  { 0, 0, backend::BindingType::TextureSampler, 1, "uSource" },
  { 0, 1, backend::BindingType::TextureSampler, 1, "uDestination" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform CompositeUniforms
{
    highp float uAlpha;
    mediump int uBlendMode;
    highp float uTextR;
    highp float uTextG;
    highp float uTextB;
    highp float uGamma;
    highp float uContrast;
    mediump int uLinearText;
} uComposite;

uniform mediump sampler2D uSource;
uniform mediump sampler2D uDestination;

in highp vec2 vTexCoord;
layout(location = 0) out highp vec4 oColor;

highp vec3 unpremultiplied(highp vec4 colour)
{
    if (colour.w <= 0.0)
    {
        return vec3(0.0);
    }
    return colour.xyz / vec3(colour.w);
}

highp float saturationOf(highp vec3 colour)
{
    return max(max(colour.x, colour.y), colour.z) - min(min(colour.x, colour.y), colour.z);
}

highp float luminosityOf(highp vec3 colour)
{
    return ((0.300000011920928955078125 * colour.x) + (0.589999973773956298828125 * colour.y)) + (0.10999999940395355224609375 * colour.z);
}

highp vec3 withLuminosity(inout highp vec3 colour, highp float luminosity)
{
    highp float delta = luminosity - luminosityOf(colour);
    colour += vec3(delta);
    if (colour.x < 0.0)
    {
        colour.y -= ((colour.x * 0.589999973773956298828125) / 0.300000011920928955078125);
        colour.z -= ((colour.x * 0.10999999940395355224609375) / 0.300000011920928955078125);
        colour.x = 0.0;
    }
    if (colour.y < 0.0)
    {
        colour.x -= ((colour.y * 0.300000011920928955078125) / 0.589999973773956298828125);
        colour.z -= ((colour.y * 0.10999999940395355224609375) / 0.589999973773956298828125);
        colour.y = 0.0;
    }
    if (colour.z < 0.0)
    {
        colour.x -= ((colour.z * 0.300000011920928955078125) / 0.10999999940395355224609375);
        colour.y -= ((colour.z * 0.589999973773956298828125) / 0.10999999940395355224609375);
        colour.z = 0.0;
    }
    highp float maximum = max(max(colour.x, colour.y), colour.z);
    if (maximum > 1.0)
    {
        colour /= vec3(maximum);
    }
    return colour;
}

highp vec3 withSaturation(highp vec3 colour, highp float saturation)
{
    highp float current = saturationOf(colour);
    if (current <= 0.0)
    {
        return vec3(luminosityOf(colour));
    }
    highp float scale = saturation / current;
    highp float minimum = min(min(colour.x, colour.y), colour.z);
    highp vec3 result = vec3(minimum) + ((colour - vec3(minimum)) * scale);
    bool _387 = result.x > 1.0;
    bool _394;
    if (!_387)
    {
        _394 = result.y > 1.0;
    }
    else
    {
        _394 = _387;
    }
    bool _401;
    if (!_394)
    {
        _401 = result.z > 1.0;
    }
    else
    {
        _401 = _394;
    }
    if (_401)
    {
        highp vec3 param = result;
        highp vec3 _408 = withLuminosity(param, luminosityOf(colour));
        result = _408;
    }
    return result;
}

highp vec3 nonSeparableBlend(mediump int mode, highp vec3 d, highp vec3 s)
{
    switch (mode)
    {
        case 25:
        {
            highp vec3 param = d;
            highp vec3 result = withSaturation(param, saturationOf(s));
            highp vec3 param_1 = result;
            highp vec3 _426 = withLuminosity(param_1, luminosityOf(result));
            return _426;
        }
        case 26:
        {
            highp vec3 param_2 = d;
            return withSaturation(param_2, saturationOf(s));
        }
        case 27:
        {
            highp vec3 param_3 = d;
            highp vec3 _434 = withLuminosity(param_3, luminosityOf(s));
            return _434;
        }
        case 28:
        {
            highp vec3 param_4 = d;
            highp vec3 _438 = withLuminosity(param_4, luminosityOf(d));
            return _438;
        }
        default:
        {
            return d;
        }
    }
}

highp vec3 hardLight(highp vec3 base, highp vec3 blend)
{
    return mix((blend * 2.0) * base, vec3(1.0) - (((vec3(1.0) - blend) * 2.0) * (vec3(1.0) - base)), step(vec3(0.5), blend));
}

highp float colorDodgeChannel(highp float d, highp float s)
{
    if (d <= 0.0)
    {
        return 0.0;
    }
    if (s >= 1.0)
    {
        return 1.0;
    }
    return min(1.0, d / (1.0 - s));
}

highp float colorBurnChannel(highp float d, highp float s)
{
    if (d >= 1.0)
    {
        return 1.0;
    }
    if (s <= 0.0)
    {
        return 0.0;
    }
    return 1.0 - min(1.0, (1.0 - d) / s);
}

highp float softLightChannel(highp float d, highp float s)
{
    if (s <= 0.5)
    {
        return d - (((1.0 - (2.0 * s)) * d) * (1.0 - d));
    }
    highp float _140;
    if (d <= 0.25)
    {
        _140 = ((((16.0 * d) - 12.0) * d) + 4.0) * d;
    }
    else
    {
        _140 = sqrt(d);
    }
    highp float curve = _140;
    return d + (((2.0 * s) - 1.0) * (curve - d));
}

highp vec3 separableBlend(mediump int mode, highp vec3 d, highp vec3 s)
{
    switch (mode)
    {
        case 15:
        {
            return hardLight(d, s);
        }
        case 16:
        {
            return min(s, d);
        }
        case 17:
        {
            return max(s, d);
        }
        case 18:
        {
            return vec3(colorDodgeChannel(d.x, s.x), colorDodgeChannel(d.y, s.y), colorDodgeChannel(d.z, s.z));
        }
        case 19:
        {
            return vec3(colorBurnChannel(d.x, s.x), colorBurnChannel(d.y, s.y), colorBurnChannel(d.z, s.z));
        }
        case 20:
        {
            return hardLight(s, d);
        }
        case 21:
        {
            return vec3(softLightChannel(d.x, s.x), softLightChannel(d.y, s.y), softLightChannel(d.z, s.z));
        }
        case 22:
        {
            return abs(s - d);
        }
        case 23:
        {
            return (s + d) - ((s * 2.0) * d);
        }
        case 24:
        {
            return s * d;
        }
        default:
        {
            return s;
        }
    }
}

highp vec3 colourFunction(mediump int mode, highp vec3 d, highp vec3 s)
{
    if (mode <= 14)
    {
        return s;
    }
    if (mode >= 25)
    {
        return nonSeparableBlend(mode, d, s);
    }
    return clamp(separableBlend(mode, d, s), vec3(0.0), vec3(1.0));
}

void main()
{
    highp vec4 source = texture(uSource, vTexCoord);
    if (uComposite.uLinearText != 0)
    {
        highp vec4 destination = texture(uDestination, vTexCoord);
        highp vec3 coverage = clamp(source.xyz, vec3(0.0), vec3(1.0)) * clamp(uComposite.uAlpha, 0.0, 1.0);
        highp vec3 text = clamp(vec3(uComposite.uTextR, uComposite.uTextG, uComposite.uTextB), vec3(0.0), vec3(1.0));
        highp vec3 _519;
        if (destination.w > 9.9999997473787516355514526367188e-05)
        {
            _519 = destination.xyz / vec3(destination.w);
        }
        else
        {
            _519 = vec3(0.0);
        }
        highp vec3 background = _519;
        highp vec3 shaped = pow(max(coverage, vec3(0.0)), vec3(1.0 / uComposite.uGamma));
        shaped = (shaped * (1.0 + uComposite.uContrast)) / ((shaped * uComposite.uContrast) + vec3(1.0));
        highp float textAlpha = max(max(shaped.x, shaped.y), shaped.z);
        highp vec3 premultiplied = (text * shaped) + ((background * destination.w) * (vec3(1.0) - shaped));
        highp float outAlpha = textAlpha + (destination.w * (1.0 - textAlpha));
        oColor = vec4(clamp(premultiplied, vec3(0.0), vec3(max(outAlpha, 0.0))), clamp(outAlpha, 0.0, 1.0));
        return;
    }
    source *= clamp(uComposite.uAlpha, 0.0, 1.0);
    highp vec4 destination_1 = texture(uDestination, vTexCoord);
    highp float sa = source.w;
    highp float da = destination_1.w;
    highp vec3 blended = colourFunction(uComposite.uBlendMode, unpremultiplied(destination_1), unpremultiplied(source));
    highp float outAlpha_1 = sa + (da * (1.0 - sa));
    oColor = vec4((destination_1.xyz * (1.0 - sa)) + (blended * sa), outAlpha_1);
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform CompositeUniforms
{
    float uAlpha;
    int uBlendMode;
    float uTextR;
    float uTextG;
    float uTextB;
    float uGamma;
    float uContrast;
    int uLinearText;
} uComposite;

 uniform sampler2D uSource;
 uniform sampler2D uDestination;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oColor;

vec3 unpremultiplied(vec4 colour)
{
    if (colour.w <= 0.0)
    {
        return vec3(0.0);
    }
    return colour.xyz / vec3(colour.w);
}

float saturationOf(vec3 colour)
{
    return max(max(colour.x, colour.y), colour.z) - min(min(colour.x, colour.y), colour.z);
}

float luminosityOf(vec3 colour)
{
    return ((0.300000011920928955078125 * colour.x) + (0.589999973773956298828125 * colour.y)) + (0.10999999940395355224609375 * colour.z);
}

vec3 withLuminosity(inout vec3 colour, float luminosity)
{
    float delta = luminosity - luminosityOf(colour);
    colour += vec3(delta);
    if (colour.x < 0.0)
    {
        colour.y -= ((colour.x * 0.589999973773956298828125) / 0.300000011920928955078125);
        colour.z -= ((colour.x * 0.10999999940395355224609375) / 0.300000011920928955078125);
        colour.x = 0.0;
    }
    if (colour.y < 0.0)
    {
        colour.x -= ((colour.y * 0.300000011920928955078125) / 0.589999973773956298828125);
        colour.z -= ((colour.y * 0.10999999940395355224609375) / 0.589999973773956298828125);
        colour.y = 0.0;
    }
    if (colour.z < 0.0)
    {
        colour.x -= ((colour.z * 0.300000011920928955078125) / 0.10999999940395355224609375);
        colour.y -= ((colour.z * 0.589999973773956298828125) / 0.10999999940395355224609375);
        colour.z = 0.0;
    }
    float maximum = max(max(colour.x, colour.y), colour.z);
    if (maximum > 1.0)
    {
        colour /= vec3(maximum);
    }
    return colour;
}

vec3 withSaturation(vec3 colour, float saturation)
{
    float current = saturationOf(colour);
    if (current <= 0.0)
    {
        return vec3(luminosityOf(colour));
    }
    float scale = saturation / current;
    float minimum = min(min(colour.x, colour.y), colour.z);
    vec3 result = vec3(minimum) + ((colour - vec3(minimum)) * scale);
    bool _387 = result.x > 1.0;
    bool _394;
    if (!_387)
    {
        _394 = result.y > 1.0;
    }
    else
    {
        _394 = _387;
    }
    bool _401;
    if (!_394)
    {
        _401 = result.z > 1.0;
    }
    else
    {
        _401 = _394;
    }
    if (_401)
    {
        vec3 param = result;
        vec3 _408 = withLuminosity(param, luminosityOf(colour));
        result = _408;
    }
    return result;
}

vec3 nonSeparableBlend(int mode, vec3 d, vec3 s)
{
    switch (mode)
    {
        case 25:
        {
            vec3 param = d;
            vec3 result = withSaturation(param, saturationOf(s));
            vec3 param_1 = result;
            vec3 _426 = withLuminosity(param_1, luminosityOf(result));
            return _426;
        }
        case 26:
        {
            vec3 param_2 = d;
            return withSaturation(param_2, saturationOf(s));
        }
        case 27:
        {
            vec3 param_3 = d;
            vec3 _434 = withLuminosity(param_3, luminosityOf(s));
            return _434;
        }
        case 28:
        {
            vec3 param_4 = d;
            vec3 _438 = withLuminosity(param_4, luminosityOf(d));
            return _438;
        }
        default:
        {
            return d;
        }
    }
}

vec3 hardLight(vec3 base, vec3 blend)
{
    return mix((blend * 2.0) * base, vec3(1.0) - (((vec3(1.0) - blend) * 2.0) * (vec3(1.0) - base)), step(vec3(0.5), blend));
}

float colorDodgeChannel(float d, float s)
{
    if (d <= 0.0)
    {
        return 0.0;
    }
    if (s >= 1.0)
    {
        return 1.0;
    }
    return min(1.0, d / (1.0 - s));
}

float colorBurnChannel(float d, float s)
{
    if (d >= 1.0)
    {
        return 1.0;
    }
    if (s <= 0.0)
    {
        return 0.0;
    }
    return 1.0 - min(1.0, (1.0 - d) / s);
}

float softLightChannel(float d, float s)
{
    if (s <= 0.5)
    {
        return d - (((1.0 - (2.0 * s)) * d) * (1.0 - d));
    }
    float _140;
    if (d <= 0.25)
    {
        _140 = ((((16.0 * d) - 12.0) * d) + 4.0) * d;
    }
    else
    {
        _140 = sqrt(d);
    }
    float curve = _140;
    return d + (((2.0 * s) - 1.0) * (curve - d));
}

vec3 separableBlend(int mode, vec3 d, vec3 s)
{
    switch (mode)
    {
        case 15:
        {
            return hardLight(d, s);
        }
        case 16:
        {
            return min(s, d);
        }
        case 17:
        {
            return max(s, d);
        }
        case 18:
        {
            return vec3(colorDodgeChannel(d.x, s.x), colorDodgeChannel(d.y, s.y), colorDodgeChannel(d.z, s.z));
        }
        case 19:
        {
            return vec3(colorBurnChannel(d.x, s.x), colorBurnChannel(d.y, s.y), colorBurnChannel(d.z, s.z));
        }
        case 20:
        {
            return hardLight(s, d);
        }
        case 21:
        {
            return vec3(softLightChannel(d.x, s.x), softLightChannel(d.y, s.y), softLightChannel(d.z, s.z));
        }
        case 22:
        {
            return abs(s - d);
        }
        case 23:
        {
            return (s + d) - ((s * 2.0) * d);
        }
        case 24:
        {
            return s * d;
        }
        default:
        {
            return s;
        }
    }
}

vec3 colourFunction(int mode, vec3 d, vec3 s)
{
    if (mode <= 14)
    {
        return s;
    }
    if (mode >= 25)
    {
        return nonSeparableBlend(mode, d, s);
    }
    return clamp(separableBlend(mode, d, s), vec3(0.0), vec3(1.0));
}

void main()
{
    vec4 source = texture(uSource, vTexCoord);
    if (uComposite.uLinearText != 0)
    {
        vec4 destination = texture(uDestination, vTexCoord);
        vec3 coverage = clamp(source.xyz, vec3(0.0), vec3(1.0)) * clamp(uComposite.uAlpha, 0.0, 1.0);
        vec3 text = clamp(vec3(uComposite.uTextR, uComposite.uTextG, uComposite.uTextB), vec3(0.0), vec3(1.0));
        vec3 _519;
        if (destination.w > 9.9999997473787516355514526367188e-05)
        {
            _519 = destination.xyz / vec3(destination.w);
        }
        else
        {
            _519 = vec3(0.0);
        }
        vec3 background = _519;
        vec3 shaped = pow(max(coverage, vec3(0.0)), vec3(1.0 / uComposite.uGamma));
        shaped = (shaped * (1.0 + uComposite.uContrast)) / ((shaped * uComposite.uContrast) + vec3(1.0));
        float textAlpha = max(max(shaped.x, shaped.y), shaped.z);
        vec3 premultiplied = (text * shaped) + ((background * destination.w) * (vec3(1.0) - shaped));
        float outAlpha = textAlpha + (destination.w * (1.0 - textAlpha));
        oColor = vec4(clamp(premultiplied, vec3(0.0), vec3(max(outAlpha, 0.0))), clamp(outAlpha, 0.0, 1.0));
        return;
    }
    source *= clamp(uComposite.uAlpha, 0.0, 1.0);
    vec4 destination_1 = texture(uDestination, vTexCoord);
    float sa = source.w;
    float da = destination_1.w;
    vec3 blended = colourFunction(uComposite.uBlendMode, unpremultiplied(destination_1), unpremultiplied(source));
    float outAlpha_1 = sa + (da * (1.0 - sa));
    oColor = vec4((destination_1.xyz * (1.0 - sa)) + (blended * sa), outAlpha_1);
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "composite.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace composite_frag

// composite.vert: 300 ES bytes, 417 desktop bytes.
namespace composite_vert {
inline constexpr const backend::ShaderBindingDeclaration* kBindings = nullptr;
inline constexpr size_t kBindingCount = 0;
inline constexpr const char* kEs = R"GLSL(#version 300 es

out vec2 vTexCoord;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) in vec2 aPosition;

void main()
{
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) in vec2 aPosition;

void main()
{
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Vertex, "composite.vert", kEs, kDesktop, nullptr, 0 };
} // namespace composite_vert

// coverage.frag: 582 ES bytes, 655 desktop bytes.
namespace coverage_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 2, backend::BindingType::UniformBuffer, 1, "FieldUniforms" },
  { 0, 0, backend::BindingType::TextureSampler, 1, "uCoverageField" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform FieldUniforms
{
    highp vec4 uField;
} uField;

uniform mediump sampler2D uCoverageField;

in highp vec2 vTexCoord;
in highp vec2 vRectSize;
in highp vec4 vParams;
in highp vec4 vColor;
layout(location = 0) out highp vec4 oColor;

void main()
{
    highp vec2 coordinate = ((vTexCoord * vRectSize) + vParams.xy) / uField.uField.xy;
    highp float coverage = texture(uCoverageField, coordinate).x;
    highp vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    oColor = paint * coverage;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform FieldUniforms
{
    vec4 uField;
} uField;

 uniform sampler2D uCoverageField;

layout(location = 0) in vec2 vTexCoord;
layout(location = 3) in vec2 vRectSize;
layout(location = 2) in vec4 vParams;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 oColor;

void main()
{
    vec2 coordinate = ((vTexCoord * vRectSize) + vParams.xy) / uField.uField.xy;
    float coverage = texture(uCoverageField, coordinate).x;
    vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    oColor = paint * coverage;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "coverage.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace coverage_frag

// gradient.frag: 3932 ES bytes, 3784 desktop bytes.
namespace gradient_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 2, backend::BindingType::UniformBuffer, 1, "GradientUniforms" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform GradientUniforms
{
    highp vec4 uPoints[2];
    highp vec4 uShape;
    highp vec4 uMatrix[2];
    mediump ivec4 uParams;
    highp vec4 uStopColor[4];
    highp vec4 uStopAlpha[4];
} uGradient;

in highp vec2 vTexCoord;
in highp vec2 vRectSize;
in highp vec4 vParams;
in highp vec4 vColor;
layout(location = 0) out highp vec4 oColor;

highp float tile(highp float value, mediump int mode)
{
    if (mode == 1)
    {
        return value - floor(value);
    }
    if (mode == 2)
    {
        highp float folded = abs((fract(value * 0.5) * 2.0) - 1.0);
        return folded;
    }
    if (mode == 3)
    {
        return ((value < 0.0) || (value > 1.0)) ? (-1.0) : value;
    }
    return clamp(value, 0.0, 1.0);
}

highp vec4 sampleStops(highp float t)
{
    highp vec4 previousStop = uGradient.uStopColor[0];
    highp float previousAlpha = uGradient.uStopAlpha[0].x;
    highp float previousOffset = previousStop.x;
    highp float _119;
    for (mediump int i = 1; i < 4; i++)
    {
        if (i >= uGradient.uParams.y)
        {
            break;
        }
        highp vec4 stop = uGradient.uStopColor[i];
        highp float stopOffset = stop.x;
        if (t <= stopOffset)
        {
            highp float span = stopOffset - previousOffset;
            if (span > 0.0)
            {
                _119 = (t - previousOffset) / span;
            }
            else
            {
                _119 = 0.0;
            }
            highp float weight = _119;
            highp vec4 blended = mix(vec4(previousStop.yzw, previousAlpha), vec4(stop.yzw, uGradient.uStopAlpha[i].x), vec4(weight));
            return vec4(blended.xyz * blended.w, blended.w);
        }
        previousStop = stop;
        previousAlpha = uGradient.uStopAlpha[i].x;
        previousOffset = stopOffset;
    }
    highp vec4 last = uGradient.uStopColor[max((uGradient.uParams.y - 1), 0)];
    highp float lastAlpha = uGradient.uStopAlpha[max((uGradient.uParams.y - 1), 0)].x;
    return vec4(last.yzw * lastAlpha, lastAlpha);
}

void main()
{
    highp vec2 device = (vTexCoord * vRectSize) - vec2(vParams.y);
    highp vec2 gradientSpace = vec2(((uGradient.uMatrix[0].x * device.x) + (uGradient.uMatrix[0].y * device.y)) + uGradient.uMatrix[1].x, ((uGradient.uMatrix[0].z * device.x) + (uGradient.uMatrix[0].w * device.y)) + uGradient.uMatrix[1].y);
    highp float t;
    if (uGradient.uShape.x > 0.0)
    {
        t = length(gradientSpace - uGradient.uPoints[0].xy) / uGradient.uShape.x;
    }
    else
    {
        highp vec2 axis = uGradient.uPoints[1].xy - uGradient.uPoints[0].xy;
        highp float lengthSquared = dot(axis, axis);
        highp float _270;
        if (lengthSquared > 0.0)
        {
            _270 = dot(gradientSpace - uGradient.uPoints[0].xy, axis) / lengthSquared;
        }
        else
        {
            _270 = 0.0;
        }
        t = _270;
    }
    highp float tiled = tile(t, uGradient.uParams.x);
    highp vec4 gradient = sampleStops(tiled);
    highp vec4 fill = gradient * vColor.w;
    highp float coverage = 1.0;
    if (vParams.z > 0.5)
    {
        highp vec2 size = vRectSize - vec2(2.0 * vParams.y);
        highp vec2 halfSize = size * 0.5;
        highp vec2 centred = (vTexCoord * vRectSize) - (vRectSize * 0.5);
        highp float clamped = min(vParams.x, min(halfSize.x, halfSize.y));
        highp vec2 distanceToInner = abs(centred) - (halfSize - vec2(clamped));
        highp float _distance = (length(max(distanceToInner, vec2(0.0))) + min(max(distanceToInner.x, distanceToInner.y), 0.0)) - clamped;
        highp float _355;
        if (vParams.y <= 0.0)
        {
            _355 = float(_distance <= 0.0);
        }
        else
        {
            _355 = clamp(0.5 - (_distance / vParams.y), 0.0, 1.0);
        }
        coverage = _355;
    }
    oColor = fill * coverage;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform GradientUniforms
{
    vec4 uPoints[2];
    vec4 uShape;
    vec4 uMatrix[2];
    ivec4 uParams;
    vec4 uStopColor[4];
    vec4 uStopAlpha[4];
} uGradient;

layout(location = 0) in vec2 vTexCoord;
layout(location = 3) in vec2 vRectSize;
layout(location = 2) in vec4 vParams;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 oColor;

float tile(float value, int mode)
{
    if (mode == 1)
    {
        return value - floor(value);
    }
    if (mode == 2)
    {
        float folded = abs((fract(value * 0.5) * 2.0) - 1.0);
        return folded;
    }
    if (mode == 3)
    {
        return ((value < 0.0) || (value > 1.0)) ? (-1.0) : value;
    }
    return clamp(value, 0.0, 1.0);
}

vec4 sampleStops(float t)
{
    vec4 previousStop = uGradient.uStopColor[0];
    float previousAlpha = uGradient.uStopAlpha[0].x;
    float previousOffset = previousStop.x;
    float _119;
    for (int i = 1; i < 4; i++)
    {
        if (i >= uGradient.uParams.y)
        {
            break;
        }
        vec4 stop = uGradient.uStopColor[i];
        float stopOffset = stop.x;
        if (t <= stopOffset)
        {
            float span = stopOffset - previousOffset;
            if (span > 0.0)
            {
                _119 = (t - previousOffset) / span;
            }
            else
            {
                _119 = 0.0;
            }
            float weight = _119;
            vec4 blended = mix(vec4(previousStop.yzw, previousAlpha), vec4(stop.yzw, uGradient.uStopAlpha[i].x), vec4(weight));
            return vec4(blended.xyz * blended.w, blended.w);
        }
        previousStop = stop;
        previousAlpha = uGradient.uStopAlpha[i].x;
        previousOffset = stopOffset;
    }
    vec4 last = uGradient.uStopColor[max((uGradient.uParams.y - 1), 0)];
    float lastAlpha = uGradient.uStopAlpha[max((uGradient.uParams.y - 1), 0)].x;
    return vec4(last.yzw * lastAlpha, lastAlpha);
}

void main()
{
    vec2 device = (vTexCoord * vRectSize) - vec2(vParams.y);
    vec2 gradientSpace = vec2(((uGradient.uMatrix[0].x * device.x) + (uGradient.uMatrix[0].y * device.y)) + uGradient.uMatrix[1].x, ((uGradient.uMatrix[0].z * device.x) + (uGradient.uMatrix[0].w * device.y)) + uGradient.uMatrix[1].y);
    float t;
    if (uGradient.uShape.x > 0.0)
    {
        t = length(gradientSpace - uGradient.uPoints[0].xy) / uGradient.uShape.x;
    }
    else
    {
        vec2 axis = uGradient.uPoints[1].xy - uGradient.uPoints[0].xy;
        float lengthSquared = dot(axis, axis);
        float _270;
        if (lengthSquared > 0.0)
        {
            _270 = dot(gradientSpace - uGradient.uPoints[0].xy, axis) / lengthSquared;
        }
        else
        {
            _270 = 0.0;
        }
        t = _270;
    }
    float tiled = tile(t, uGradient.uParams.x);
    vec4 gradient = sampleStops(tiled);
    vec4 fill = gradient * vColor.w;
    float coverage = 1.0;
    if (vParams.z > 0.5)
    {
        vec2 size = vRectSize - vec2(2.0 * vParams.y);
        vec2 halfSize = size * 0.5;
        vec2 centred = (vTexCoord * vRectSize) - (vRectSize * 0.5);
        float clamped = min(vParams.x, min(halfSize.x, halfSize.y));
        vec2 distanceToInner = abs(centred) - (halfSize - vec2(clamped));
        float _distance = (length(max(distanceToInner, vec2(0.0))) + min(max(distanceToInner.x, distanceToInner.y), 0.0)) - clamped;
        float _355;
        if (vParams.y <= 0.0)
        {
            _355 = float(_distance <= 0.0);
        }
        else
        {
            _355 = clamp(0.5 - (_distance / vParams.y), 0.0, 1.0);
        }
        coverage = _355;
    }
    oColor = fill * coverage;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "gradient.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace gradient_frag

// image.frag: 1754 ES bytes, 1734 desktop bytes.
namespace image_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 1, backend::BindingType::UniformBuffer, 1, "CanvasUniforms" },
  { 0, 0, backend::BindingType::TextureSampler, 1, "uTexture" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform CanvasUniforms
{
    highp vec2 uViewport;
    highp vec2 uScale;
    highp vec2 uOffset;
    highp vec2 uPadding;
    highp vec4 uImageFilter;
    highp float uOutputPq;
} uCanvas;

uniform mediump sampler2D uTexture;

in highp vec4 vColor;
in highp vec4 vParams;
in highp vec2 vTexCoord;
layout(location = 0) out highp vec4 oColor;

highp vec3 pqFromSrgb(highp vec3 encoded)
{
    highp vec3 linear = mix(encoded / vec3(12.9200000762939453125), pow((encoded + vec3(0.054999999701976776123046875)) / vec3(1.05499994754791259765625), vec3(2.400000095367431640625)), step(vec3(0.040449999272823333740234375), encoded));
    linear = clamp(mat3(vec3(0.627399981021881103515625, 0.069099999964237213134765625, 0.01640000008046627044677734375), vec3(0.329299986362457275390625, 0.91949999332427978515625, 0.087999999523162841796875), vec3(0.0432999990880489349365234375, 0.011400000192224979400634765625, 0.895600020885467529296875)) * linear, vec3(0.0), vec3(1.0));
    highp vec3 nits = linear * 203.0;
    highp vec3 l = clamp(nits / vec3(10000.0), vec3(0.0), vec3(1.0));
    highp vec3 p = pow(l, vec3(0.1593017578125));
    return pow((vec3(0.8359375) + (p * 18.8515625)) / (vec3(1.0) + (p * 18.6875)), vec3(78.84375));
}

void main()
{
    highp vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    highp vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    highp vec4 sampled = texture(uTexture, uv) * paint;
    highp vec4 _143;
    if (uCanvas.uOutputPq > 0.5)
    {
        highp vec3 param = clamp(sampled.xyz, vec3(0.0), vec3(1.0));
        _143 = vec4(pqFromSrgb(param), sampled.w);
    }
    else
    {
        _143 = sampled;
    }
    oColor = _143;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform CanvasUniforms
{
    vec2 uViewport;
    vec2 uScale;
    vec2 uOffset;
    vec2 uPadding;
    vec4 uImageFilter;
    float uOutputPq;
} uCanvas;

 uniform sampler2D uTexture;

layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oColor;

vec3 pqFromSrgb(vec3 encoded)
{
    vec3 linear = mix(encoded / vec3(12.9200000762939453125), pow((encoded + vec3(0.054999999701976776123046875)) / vec3(1.05499994754791259765625), vec3(2.400000095367431640625)), step(vec3(0.040449999272823333740234375), encoded));
    linear = clamp(mat3(vec3(0.627399981021881103515625, 0.069099999964237213134765625, 0.01640000008046627044677734375), vec3(0.329299986362457275390625, 0.91949999332427978515625, 0.087999999523162841796875), vec3(0.0432999990880489349365234375, 0.011400000192224979400634765625, 0.895600020885467529296875)) * linear, vec3(0.0), vec3(1.0));
    vec3 nits = linear * 203.0;
    vec3 l = clamp(nits / vec3(10000.0), vec3(0.0), vec3(1.0));
    vec3 p = pow(l, vec3(0.1593017578125));
    return pow((vec3(0.8359375) + (p * 18.8515625)) / (vec3(1.0) + (p * 18.6875)), vec3(78.84375));
}

void main()
{
    vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    vec4 sampled = texture(uTexture, uv) * paint;
    vec4 _143;
    if (uCanvas.uOutputPq > 0.5)
    {
        vec3 param = clamp(sampled.xyz, vec3(0.0), vec3(1.0));
        _143 = vec4(pqFromSrgb(param), sampled.w);
    }
    else
    {
        _143 = sampled;
    }
    oColor = _143;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "image.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace image_frag

// image_cubic.frag: 2285 ES bytes, 2157 desktop bytes.
namespace image_cubic_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 1, backend::BindingType::UniformBuffer, 1, "CanvasUniforms" },
  { 0, 0, backend::BindingType::TextureSampler, 1, "uTexture" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform CanvasUniforms
{
    highp vec2 uViewport;
    highp vec2 uScale;
    highp vec2 uOffset;
    highp vec2 uPadding;
    highp vec4 uImageFilter;
    highp float uOutputPq;
} uCanvas;

uniform mediump sampler2D uTexture;

in highp vec4 vColor;
in highp vec4 vParams;
in highp vec2 vTexCoord;
layout(location = 0) out highp vec4 oColor;

highp float cubicWeight(highp float x, highp float b, highp float c)
{
    highp float ax = abs(x);
    if (ax < 1.0)
    {
        return (((((((12.0 - (9.0 * b)) - (6.0 * c)) * ax) * ax) * ax) + (((((-18.0) + (12.0 * b)) + (6.0 * c)) * ax) * ax)) + (6.0 - (2.0 * b))) / 6.0;
    }
    if (ax < 2.0)
    {
        return ((((((((-b) - (6.0 * c)) * ax) * ax) * ax) + ((((6.0 * b) + (30.0 * c)) * ax) * ax)) + ((((-12.0) * b) - (48.0 * c)) * ax)) + ((8.0 * b) + (24.0 * c))) / 6.0;
    }
    return 0.0;
}

void main()
{
    highp vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    highp vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    highp float b = uCanvas.uImageFilter.x;
    highp float c = uCanvas.uImageFilter.y;
    highp vec2 size = vec2(textureSize(uTexture, 0));
    highp vec2 pixel = (uv * size) - vec2(0.5);
    highp vec2 base = floor(pixel);
    highp vec2 fraction = pixel - base;
    highp float sumX = 0.0;
    highp float sumY = 0.0;
    highp float wx[4];
    highp float wy[4];
    for (mediump int i = 0; i < 4; i++)
    {
        highp float param = (float(i) - 1.0) - fraction.x;
        highp float param_1 = b;
        highp float param_2 = c;
        wx[i] = cubicWeight(param, param_1, param_2);
        highp float param_3 = (float(i) - 1.0) - fraction.y;
        highp float param_4 = b;
        highp float param_5 = c;
        wy[i] = cubicWeight(param_3, param_4, param_5);
        sumX += wx[i];
        sumY += wy[i];
    }
    highp vec4 sum = vec4(0.0);
    for (mediump int j = 0; j < 4; j++)
    {
        for (mediump int i_1 = 0; i_1 < 4; i_1++)
        {
            highp vec2 coord = clamp(base + vec2(float(i_1) - 1.0, float(j) - 1.0), vec2(0.0), size - vec2(1.0));
            sum += (texelFetch(uTexture, ivec2(coord), 0) * ((wx[i_1] / sumX) * (wy[j] / sumY)));
        }
    }
    oColor = sum * paint;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform CanvasUniforms
{
    vec2 uViewport;
    vec2 uScale;
    vec2 uOffset;
    vec2 uPadding;
    vec4 uImageFilter;
    float uOutputPq;
} uCanvas;

 uniform sampler2D uTexture;

layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oColor;

float cubicWeight(float x, float b, float c)
{
    float ax = abs(x);
    if (ax < 1.0)
    {
        return (((((((12.0 - (9.0 * b)) - (6.0 * c)) * ax) * ax) * ax) + (((((-18.0) + (12.0 * b)) + (6.0 * c)) * ax) * ax)) + (6.0 - (2.0 * b))) / 6.0;
    }
    if (ax < 2.0)
    {
        return ((((((((-b) - (6.0 * c)) * ax) * ax) * ax) + ((((6.0 * b) + (30.0 * c)) * ax) * ax)) + ((((-12.0) * b) - (48.0 * c)) * ax)) + ((8.0 * b) + (24.0 * c))) / 6.0;
    }
    return 0.0;
}

void main()
{
    vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    float b = uCanvas.uImageFilter.x;
    float c = uCanvas.uImageFilter.y;
    vec2 size = vec2(textureSize(uTexture, 0));
    vec2 pixel = (uv * size) - vec2(0.5);
    vec2 base = floor(pixel);
    vec2 fraction = pixel - base;
    float sumX = 0.0;
    float sumY = 0.0;
    float wx[4];
    float wy[4];
    for (int i = 0; i < 4; i++)
    {
        float param = (float(i) - 1.0) - fraction.x;
        float param_1 = b;
        float param_2 = c;
        wx[i] = cubicWeight(param, param_1, param_2);
        float param_3 = (float(i) - 1.0) - fraction.y;
        float param_4 = b;
        float param_5 = c;
        wy[i] = cubicWeight(param_3, param_4, param_5);
        sumX += wx[i];
        sumY += wy[i];
    }
    vec4 sum = vec4(0.0);
    for (int j = 0; j < 4; j++)
    {
        for (int i_1 = 0; i_1 < 4; i_1++)
        {
            vec2 coord = clamp(base + vec2(float(i_1) - 1.0, float(j) - 1.0), vec2(0.0), size - vec2(1.0));
            sum += (texelFetch(uTexture, ivec2(coord), 0) * ((wx[i_1] / sumX) * (wy[j] / sumY)));
        }
    }
    oColor = sum * paint;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "image_cubic.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace image_cubic_frag

// mask.frag: 961 ES bytes, 956 desktop bytes.
namespace mask_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 2, backend::BindingType::UniformBuffer, 1, "MaskUniforms" },
  { 0, 0, backend::BindingType::TextureSampler, 1, "uLayer" },
  { 0, 1, backend::BindingType::TextureSampler, 1, "uMask" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform MaskUniforms
{
    highp vec2 uMaskScale;
    highp vec2 uMaskOffset;
    mediump int uMaskMode;
} uMaskParams;

uniform mediump sampler2D uLayer;
uniform mediump sampler2D uMask;

in highp vec2 vTexCoord;
layout(location = 0) out highp vec4 oColor;

void main()
{
    highp vec4 layer = texture(uLayer, vTexCoord);
    highp vec4 mask = texture(uMask, (vTexCoord * uMaskParams.uMaskScale) + uMaskParams.uMaskOffset);
    highp float coverage = mask.w;
    if (uMaskParams.uMaskMode == 1)
    {
        highp vec3 _59;
        if (mask.w > 0.0)
        {
            _59 = mask.xyz / vec3(mask.w);
        }
        else
        {
            _59 = vec3(0.0);
        }
        highp vec3 colour = _59;
        coverage = dot(colour, vec3(0.2125000059604644775390625, 0.7153999805450439453125, 0.07209999859333038330078125)) * mask.w;
    }
    oColor = layer * coverage;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform MaskUniforms
{
    vec2 uMaskScale;
    vec2 uMaskOffset;
    int uMaskMode;
} uMaskParams;

 uniform sampler2D uLayer;
 uniform sampler2D uMask;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oColor;

void main()
{
    vec4 layer = texture(uLayer, vTexCoord);
    vec4 mask = texture(uMask, (vTexCoord * uMaskParams.uMaskScale) + uMaskParams.uMaskOffset);
    float coverage = mask.w;
    if (uMaskParams.uMaskMode == 1)
    {
        vec3 _59;
        if (mask.w > 0.0)
        {
            _59 = mask.xyz / vec3(mask.w);
        }
        else
        {
            _59 = vec3(0.0);
        }
        vec3 colour = _59;
        coverage = dot(colour, vec3(0.2125000059604644775390625, 0.7153999805450439453125, 0.07209999859333038330078125)) * mask.w;
    }
    oColor = layer * coverage;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "mask.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace mask_frag

// path.frag: 156 ES bytes, 227 desktop bytes.
namespace path_frag {
inline constexpr const backend::ShaderBindingDeclaration* kBindings = nullptr;
inline constexpr size_t kBindingCount = 0;
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(location = 0) out vec4 oColor;
in vec4 vColor;

void main()
{
    oColor = vColor;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(location = 0) out vec4 oColor;
layout(location = 0) in vec4 vColor;

void main()
{
    oColor = vColor;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "path.frag", kEs, kDesktop, nullptr, 0 };
} // namespace path_frag

// path.vert: 495 ES bytes, 612 desktop bytes.
namespace path_vert {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 1, backend::BindingType::UniformBuffer, 1, "CanvasUniforms" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es

layout(std140) uniform CanvasUniforms
{
    vec2 uViewport;
    vec2 uScale;
    vec2 uOffset;
    vec2 uPadding;
    vec4 uImageFilter;
    float uOutputPq;
} uCanvas;

layout(location = 0) in vec2 aPosition;
out vec4 vColor;
layout(location = 1) in vec4 aColor;

void main()
{
    gl_Position = vec4((aPosition * uCanvas.uScale) + uCanvas.uOffset, 0.0, 1.0);
    vColor = aColor;
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform CanvasUniforms
{
    vec2 uViewport;
    vec2 uScale;
    vec2 uOffset;
    vec2 uPadding;
    vec4 uImageFilter;
    float uOutputPq;
} uCanvas;

layout(location = 0) in vec2 aPosition;
layout(location = 0) out vec4 vColor;
layout(location = 1) in vec4 aColor;

void main()
{
    gl_Position = vec4((aPosition * uCanvas.uScale) + uCanvas.uOffset, 0.0, 1.0);
    vColor = aColor;
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Vertex, "path.vert", kEs, kDesktop, kBindings, kBindingCount };
} // namespace path_vert

// present.frag: 2275 ES bytes, 2219 desktop bytes.
namespace present_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 2, backend::BindingType::UniformBuffer, 1, "PresentUniforms" },
  { 0, 0, backend::BindingType::TextureSampler, 1, "uSource" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

layout(std140) uniform PresentUniforms
{
    highp vec4 uRow0;
    highp vec4 uRow1;
    highp vec4 uRow2;
    highp vec4 uParameters;
} uPresent;

uniform mediump sampler2D uSource;

in highp vec2 vTexCoord;
layout(location = 0) out highp vec4 oColor;

highp vec3 srgbToLinear(highp vec3 encoded)
{
    return mix(encoded / vec3(12.9200000762939453125), pow((encoded + vec3(0.054999999701976776123046875)) / vec3(1.05499994754791259765625), vec3(2.400000095367431640625)), step(vec3(0.040449999272823333740234375), encoded));
}

highp vec3 pqFromNits(highp vec3 nits)
{
    highp vec3 l = clamp(nits / vec3(10000.0), vec3(0.0), vec3(1.0));
    highp vec3 p = pow(l, vec3(0.1593017578125));
    return pow((vec3(0.8359375) + (p * 18.8515625)) / (vec3(1.0) + (p * 18.6875)), vec3(78.84375));
}

highp vec3 linearToSrgb(highp vec3 linear)
{
    return mix(linear * 12.9200000762939453125, (pow(linear, vec3(0.4166666567325592041015625)) * 1.05499994754791259765625) - vec3(0.054999999701976776123046875), step(vec3(0.003130800090730190277099609375), linear));
}

void main()
{
    highp vec4 sampled = texture(uSource, vTexCoord);
    highp vec3 linear = sampled.xyz;
    bool _111 = sampled.w > 9.9999997473787516355514526367188e-05;
    bool _118;
    if (_111)
    {
        _118 = sampled.w < 0.99989998340606689453125;
    }
    else
    {
        _118 = _111;
    }
    if (_118)
    {
        linear /= vec3(sampled.w);
    }
    if (uPresent.uParameters.y > 0.5)
    {
        highp vec3 param = clamp(linear, vec3(0.0), vec3(1.0));
        linear = srgbToLinear(param);
    }
    highp vec3 converted = vec3(dot(uPresent.uRow0.xyz, linear), dot(uPresent.uRow1.xyz, linear), dot(uPresent.uRow2.xyz, linear));
    converted = max(converted, vec3(0.0));
    if (uPresent.uParameters.x < 0.5)
    {
        highp vec3 param_1 = converted * uPresent.uParameters.z;
        oColor = vec4(pqFromNits(param_1), 1.0);
    }
    else
    {
        if (uPresent.uParameters.x < 1.5)
        {
            oColor = vec4(converted, 1.0);
        }
        else
        {
            highp vec3 param_2 = clamp(converted, vec3(0.0), vec3(1.0));
            oColor = vec4(linearToSrgb(param_2), 1.0);
        }
    }
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform PresentUniforms
{
    vec4 uRow0;
    vec4 uRow1;
    vec4 uRow2;
    vec4 uParameters;
} uPresent;

 uniform sampler2D uSource;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 oColor;

vec3 srgbToLinear(vec3 encoded)
{
    return mix(encoded / vec3(12.9200000762939453125), pow((encoded + vec3(0.054999999701976776123046875)) / vec3(1.05499994754791259765625), vec3(2.400000095367431640625)), step(vec3(0.040449999272823333740234375), encoded));
}

vec3 pqFromNits(vec3 nits)
{
    vec3 l = clamp(nits / vec3(10000.0), vec3(0.0), vec3(1.0));
    vec3 p = pow(l, vec3(0.1593017578125));
    return pow((vec3(0.8359375) + (p * 18.8515625)) / (vec3(1.0) + (p * 18.6875)), vec3(78.84375));
}

vec3 linearToSrgb(vec3 linear)
{
    return mix(linear * 12.9200000762939453125, (pow(linear, vec3(0.4166666567325592041015625)) * 1.05499994754791259765625) - vec3(0.054999999701976776123046875), step(vec3(0.003130800090730190277099609375), linear));
}

void main()
{
    vec4 sampled = texture(uSource, vTexCoord);
    vec3 linear = sampled.xyz;
    bool _111 = sampled.w > 9.9999997473787516355514526367188e-05;
    bool _118;
    if (_111)
    {
        _118 = sampled.w < 0.99989998340606689453125;
    }
    else
    {
        _118 = _111;
    }
    if (_118)
    {
        linear /= vec3(sampled.w);
    }
    if (uPresent.uParameters.y > 0.5)
    {
        vec3 param = clamp(linear, vec3(0.0), vec3(1.0));
        linear = srgbToLinear(param);
    }
    vec3 converted = vec3(dot(uPresent.uRow0.xyz, linear), dot(uPresent.uRow1.xyz, linear), dot(uPresent.uRow2.xyz, linear));
    converted = max(converted, vec3(0.0));
    if (uPresent.uParameters.x < 0.5)
    {
        vec3 param_1 = converted * uPresent.uParameters.z;
        oColor = vec4(pqFromNits(param_1), 1.0);
    }
    else
    {
        if (uPresent.uParameters.x < 1.5)
        {
            oColor = vec4(converted, 1.0);
        }
        else
        {
            vec3 param_2 = clamp(converted, vec3(0.0), vec3(1.0));
            oColor = vec4(linearToSrgb(param_2), 1.0);
        }
    }
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "present.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace present_frag

// quad.frag: 1459 ES bytes, 1454 desktop bytes.
namespace quad_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 0, backend::BindingType::TextureSampler, 1, "uTexture" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

uniform mediump sampler2D uTexture;

in highp vec4 vColor;
in highp vec4 vParams;
in highp vec2 vTexCoord;
in highp vec2 vRectSize;
layout(location = 0) out highp vec4 oColor;

highp float roundedRectDistance(highp vec2 point, highp vec2 size, highp float radius)
{
    highp vec2 halfSize = size * 0.5;
    highp vec2 centred = point - halfSize;
    highp float clamped = min(radius, min(halfSize.x, halfSize.y));
    highp vec2 distanceToInner = abs(centred) - (halfSize - vec2(clamped));
    return (length(max(distanceToInner, vec2(0.0))) + min(max(distanceToInner.x, distanceToInner.y), 0.0)) - clamped;
}

highp float roundedRectCoverage(highp vec2 point, highp vec2 size, highp float radius, highp float feather)
{
    if (feather <= 0.0)
    {
        return float(roundedRectDistance(point, size, radius) <= 0.0);
    }
    highp float _distance = roundedRectDistance(point, size, radius);
    return clamp(0.5 - (_distance / feather), 0.0, 1.0);
}

void main()
{
    highp vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    highp vec4 fill = paint;
    if (vParams.w > 0.5)
    {
        fill = texture(uTexture, vTexCoord) * paint;
    }
    highp float coverage = 1.0;
    if (vParams.z > 0.5)
    {
        coverage = roundedRectCoverage((vTexCoord * vRectSize) - vec2(vParams.y), vRectSize - vec2(2.0 * vParams.y), vParams.x, vParams.y);
    }
    oColor = fill * coverage;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

 uniform sampler2D uTexture;

layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 0) in vec2 vTexCoord;
layout(location = 3) in vec2 vRectSize;
layout(location = 0) out vec4 oColor;

float roundedRectDistance(vec2 point, vec2 size, float radius)
{
    vec2 halfSize = size * 0.5;
    vec2 centred = point - halfSize;
    float clamped = min(radius, min(halfSize.x, halfSize.y));
    vec2 distanceToInner = abs(centred) - (halfSize - vec2(clamped));
    return (length(max(distanceToInner, vec2(0.0))) + min(max(distanceToInner.x, distanceToInner.y), 0.0)) - clamped;
}

float roundedRectCoverage(vec2 point, vec2 size, float radius, float feather)
{
    if (feather <= 0.0)
    {
        return float(roundedRectDistance(point, size, radius) <= 0.0);
    }
    float _distance = roundedRectDistance(point, size, radius);
    return clamp(0.5 - (_distance / feather), 0.0, 1.0);
}

void main()
{
    vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    vec4 fill = paint;
    if (vParams.w > 0.5)
    {
        fill = texture(uTexture, vTexCoord) * paint;
    }
    float coverage = 1.0;
    if (vParams.z > 0.5)
    {
        coverage = roundedRectCoverage((vTexCoord * vRectSize) - vec2(vParams.y), vRectSize - vec2(2.0 * vParams.y), vParams.x, vParams.y);
    }
    oColor = fill * coverage;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "quad.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace quad_frag

// quad.vert: 1240 ES bytes, 1525 desktop bytes.
namespace quad_vert {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 1, backend::BindingType::UniformBuffer, 1, "CanvasUniforms" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es

layout(std140) uniform CanvasUniforms
{
    vec2 uViewport;
    vec2 uScale;
    vec2 uOffset;
    vec2 uPadding;
    vec4 uImageFilter;
    float uOutputPq;
} uCanvas;

layout(location = 1) in vec4 aRect;
layout(location = 0) in vec2 aCorner;
out vec2 vTexCoord;
out vec4 vColor;
layout(location = 3) in vec4 aColor;
out vec4 vParams;
layout(location = 2) in vec4 aParams;
out vec2 vRectSize;
out vec4 vSdf;
layout(location = 4) in vec4 aSdf;
out vec4 vSdfOutlineColor;
layout(location = 5) in vec4 aSdfOutlineColor;
out vec4 vSdfShadowColor;
layout(location = 6) in vec4 aSdfShadowColor;
out vec4 vSdfPattern;
layout(location = 7) in vec4 aSdfPattern;
out float vSdfFillAlpha;
layout(location = 8) in float aSdfFillAlpha;

void main()
{
    vec2 device = aRect.xy + (aCorner * aRect.zw);
    gl_Position = vec4((device * uCanvas.uScale) + uCanvas.uOffset, 0.0, 1.0);
    vTexCoord = aCorner;
    vColor = aColor;
    vParams = aParams;
    vRectSize = aRect.zw;
    vSdf = aSdf;
    vSdfOutlineColor = aSdfOutlineColor;
    vSdfShadowColor = aSdfShadowColor;
    vSdfPattern = aSdfPattern;
    vSdfFillAlpha = aSdfFillAlpha;
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(std140) uniform CanvasUniforms
{
    vec2 uViewport;
    vec2 uScale;
    vec2 uOffset;
    vec2 uPadding;
    vec4 uImageFilter;
    float uOutputPq;
} uCanvas;

layout(location = 1) in vec4 aRect;
layout(location = 0) in vec2 aCorner;
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;
layout(location = 3) in vec4 aColor;
layout(location = 2) out vec4 vParams;
layout(location = 2) in vec4 aParams;
layout(location = 3) out vec2 vRectSize;
layout(location = 4) out vec4 vSdf;
layout(location = 4) in vec4 aSdf;
layout(location = 5) out vec4 vSdfOutlineColor;
layout(location = 5) in vec4 aSdfOutlineColor;
layout(location = 6) out vec4 vSdfShadowColor;
layout(location = 6) in vec4 aSdfShadowColor;
layout(location = 7) out vec4 vSdfPattern;
layout(location = 7) in vec4 aSdfPattern;
layout(location = 8) out float vSdfFillAlpha;
layout(location = 8) in float aSdfFillAlpha;

void main()
{
    vec2 device = aRect.xy + (aCorner * aRect.zw);
    gl_Position = vec4((device * uCanvas.uScale) + uCanvas.uOffset, 0.0, 1.0);
    vTexCoord = aCorner;
    vColor = aColor;
    vParams = aParams;
    vRectSize = aRect.zw;
    vSdf = aSdf;
    vSdfOutlineColor = aSdfOutlineColor;
    vSdfShadowColor = aSdfShadowColor;
    vSdfPattern = aSdfPattern;
    vSdfFillAlpha = aSdfFillAlpha;
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Vertex, "quad.vert", kEs, kDesktop, kBindings, kBindingCount };
} // namespace quad_vert

// sdf.frag: 2481 ES bytes, 2536 desktop bytes.
namespace sdf_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 0, backend::BindingType::TextureSampler, 1, "uTexture" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

uniform mediump sampler2D uTexture;

in highp vec4 vParams;
in highp vec2 vTexCoord;
in highp vec4 vColor;
in highp vec4 vSdfPattern;
in highp float vSdfFillAlpha;
in highp vec4 vSdf;
in highp vec4 vSdfOutlineColor;
in highp vec4 vSdfShadowColor;
layout(location = 0) out highp vec4 oColor;

void main()
{
    highp vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    highp float distanceValue = texture(uTexture, uv).x;
    highp float aa = 0.0199999995529651641845703125;
    highp float fill = smoothstep(0.5 - aa, 0.5 + aa, distanceValue);
    highp vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    if (vSdfPattern.w > 0.0)
    {
        highp vec2 centred = uv - vec2(0.5);
        highp float angle = radians(vSdfPattern.z);
        highp vec2 rotated = vec2((centred.x * cos(angle)) - (centred.y * sin(angle)), (centred.x * sin(angle)) + (centred.y * cos(angle)));
        highp float mask = 0.0;
        if (vSdfPattern.x < 0.5)
        {
            mask = step(0.5, fract((rotated.x + rotated.y) * vSdfPattern.y));
        }
        else
        {
            highp vec2 grid = fract(rotated * vSdfPattern.y) - vec2(0.5);
            mask = 1.0 - step(0.25, length(grid));
        }
        paint *= (1.0 - (vSdfPattern.w * mask));
    }
    highp vec4 result = (paint * fill) * vSdfFillAlpha;
    if (vSdf.x > 0.0)
    {
        highp float outlineInner = 0.5 - vSdf.x;
        highp float outline = clamp(smoothstep(outlineInner - aa, outlineInner + aa, distanceValue) - fill, 0.0, 1.0);
        result += (vec4(vSdfOutlineColor.xyz * vSdfOutlineColor.w, vSdfOutlineColor.w) * outline);
        if (vSdf.w > 0.0)
        {
            highp float glowInner = (0.5 - vSdf.x) - vSdf.w;
            highp float glow = clamp((smoothstep(glowInner - aa, glowInner + aa, distanceValue) - fill) - outline, 0.0, 1.0);
            result += ((vec4(paint.xyz, paint.w) * glow) * 0.3499999940395355224609375);
        }
    }
    bool _228 = vSdf.y != 0.0;
    bool _235;
    if (!_228)
    {
        _235 = vSdf.z != 0.0;
    }
    else
    {
        _235 = _228;
    }
    if (_235)
    {
        highp float shadowDistance = texture(uTexture, uv - vSdf.yz).x;
        highp float shadow = smoothstep(0.5 - aa, 0.5 + aa, shadowDistance) * (1.0 - fill);
        result += (vec4(vSdfShadowColor.xyz * vSdfShadowColor.w, vSdfShadowColor.w) * shadow);
    }
    oColor = clamp(result, vec4(0.0), vec4(1.0));
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

 uniform sampler2D uTexture;

layout(location = 2) in vec4 vParams;
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 7) in vec4 vSdfPattern;
layout(location = 8) in float vSdfFillAlpha;
layout(location = 4) in vec4 vSdf;
layout(location = 5) in vec4 vSdfOutlineColor;
layout(location = 6) in vec4 vSdfShadowColor;
layout(location = 0) out vec4 oColor;

void main()
{
    vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    float distanceValue = texture(uTexture, uv).x;
    float aa = 0.0199999995529651641845703125;
    float fill = smoothstep(0.5 - aa, 0.5 + aa, distanceValue);
    vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    if (vSdfPattern.w > 0.0)
    {
        vec2 centred = uv - vec2(0.5);
        float angle = radians(vSdfPattern.z);
        vec2 rotated = vec2((centred.x * cos(angle)) - (centred.y * sin(angle)), (centred.x * sin(angle)) + (centred.y * cos(angle)));
        float mask = 0.0;
        if (vSdfPattern.x < 0.5)
        {
            mask = step(0.5, fract((rotated.x + rotated.y) * vSdfPattern.y));
        }
        else
        {
            vec2 grid = fract(rotated * vSdfPattern.y) - vec2(0.5);
            mask = 1.0 - step(0.25, length(grid));
        }
        paint *= (1.0 - (vSdfPattern.w * mask));
    }
    vec4 result = (paint * fill) * vSdfFillAlpha;
    if (vSdf.x > 0.0)
    {
        float outlineInner = 0.5 - vSdf.x;
        float outline = clamp(smoothstep(outlineInner - aa, outlineInner + aa, distanceValue) - fill, 0.0, 1.0);
        result += (vec4(vSdfOutlineColor.xyz * vSdfOutlineColor.w, vSdfOutlineColor.w) * outline);
        if (vSdf.w > 0.0)
        {
            float glowInner = (0.5 - vSdf.x) - vSdf.w;
            float glow = clamp((smoothstep(glowInner - aa, glowInner + aa, distanceValue) - fill) - outline, 0.0, 1.0);
            result += ((vec4(paint.xyz, paint.w) * glow) * 0.3499999940395355224609375);
        }
    }
    bool _228 = vSdf.y != 0.0;
    bool _235;
    if (!_228)
    {
        _235 = vSdf.z != 0.0;
    }
    else
    {
        _235 = _228;
    }
    if (_235)
    {
        float shadowDistance = texture(uTexture, uv - vSdf.yz).x;
        float shadow = smoothstep(0.5 - aa, 0.5 + aa, shadowDistance) * (1.0 - fill);
        result += (vec4(vSdfShadowColor.xyz * vSdfShadowColor.w, vSdfShadowColor.w) * shadow);
    }
    oColor = clamp(result, vec4(0.0), vec4(1.0));
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "sdf.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace sdf_frag

// sdf_pattern.frag: 2839 ES bytes, 2863 desktop bytes.
namespace sdf_pattern_frag {
inline constexpr backend::ShaderBindingDeclaration kBindings[] = {
  { 0, 0, backend::BindingType::TextureSampler, 1, "uTexture" },
  { 0, 2, backend::BindingType::TextureSampler, 1, "uPattern" },
};
inline constexpr size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);
inline constexpr const char* kEs = R"GLSL(#version 300 es
precision mediump float;
precision highp int;

uniform mediump sampler2D uTexture;
uniform mediump sampler2D uPattern;

in highp vec4 vParams;
in highp vec2 vTexCoord;
in highp vec4 vColor;
in highp vec4 vSdfPattern;
in highp float vSdfFillAlpha;
in highp vec4 vSdf;
in highp vec4 vSdfOutlineColor;
in highp vec4 vSdfShadowColor;
layout(location = 0) out highp vec4 oColor;

void main()
{
    highp vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    highp float distanceValue = texture(uTexture, uv).x;
    highp float aa = 0.0199999995529651641845703125;
    highp float fill = smoothstep(0.5 - aa, 0.5 + aa, distanceValue);
    highp vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    if (vSdfPattern.w > 0.0)
    {
        highp vec2 centred = uv - vec2(0.5);
        highp float angle = radians(vSdfPattern.z);
        highp vec2 rotated = vec2((centred.x * cos(angle)) - (centred.y * sin(angle)), (centred.x * sin(angle)) + (centred.y * cos(angle)));
        highp float mask = 0.0;
        if (vSdfPattern.x < 0.5)
        {
            mask = step(0.5, fract((rotated.x + rotated.y) * vSdfPattern.y));
        }
        else
        {
            highp vec2 grid = fract(rotated * vSdfPattern.y) - vec2(0.5);
            mask = 1.0 - step(0.25, length(grid));
        }
        paint *= (1.0 - (vSdfPattern.w * mask));
    }
    if (vSdfPattern.w > 0.0)
    {
        highp vec4 pattern = texture(uPattern, uv);
        highp vec4 _151 = paint;
        highp float _156 = paint.w;
        highp vec3 _161 = mix(_151.xyz, pattern.xyz * _156, vec3(vSdfPattern.w));
        paint.x = _161.x;
        paint.y = _161.y;
        paint.z = _161.z;
    }
    highp vec4 result = (paint * fill) * vSdfFillAlpha;
    if (vSdf.x > 0.0)
    {
        highp float outlineInner = 0.5 - vSdf.x;
        highp float outline = clamp(smoothstep(outlineInner - aa, outlineInner + aa, distanceValue) - fill, 0.0, 1.0);
        result += (vec4(vSdfOutlineColor.xyz * vSdfOutlineColor.w, vSdfOutlineColor.w) * outline);
        if (vSdf.w > 0.0)
        {
            highp float glowInner = (0.5 - vSdf.x) - vSdf.w;
            highp float glow = clamp((smoothstep(glowInner - aa, glowInner + aa, distanceValue) - fill) - outline, 0.0, 1.0);
            result += ((vec4(paint.xyz, paint.w) * glow) * 0.3499999940395355224609375);
        }
    }
    bool _255 = vSdf.y != 0.0;
    bool _262;
    if (!_255)
    {
        _262 = vSdf.z != 0.0;
    }
    else
    {
        _262 = _255;
    }
    if (_262)
    {
        highp float shadowDistance = texture(uTexture, uv - vSdf.yz).x;
        highp float shadow = smoothstep(0.5 - aa, 0.5 + aa, shadowDistance) * (1.0 - fill);
        result += (vec4(vSdfShadowColor.xyz * vSdfShadowColor.w, vSdfShadowColor.w) * shadow);
    }
    oColor = clamp(result, vec4(0.0), vec4(1.0));
}

)GLSL";
inline constexpr const char* kDesktop = R"GLSL(#version 410
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

 uniform sampler2D uTexture;
 uniform sampler2D uPattern;

layout(location = 2) in vec4 vParams;
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 7) in vec4 vSdfPattern;
layout(location = 8) in float vSdfFillAlpha;
layout(location = 4) in vec4 vSdf;
layout(location = 5) in vec4 vSdfOutlineColor;
layout(location = 6) in vec4 vSdfShadowColor;
layout(location = 0) out vec4 oColor;

void main()
{
    vec2 uv = vParams.xy + (vTexCoord * vParams.zw);
    float distanceValue = texture(uTexture, uv).x;
    float aa = 0.0199999995529651641845703125;
    float fill = smoothstep(0.5 - aa, 0.5 + aa, distanceValue);
    vec4 paint = vec4(vColor.xyz * vColor.w, vColor.w);
    if (vSdfPattern.w > 0.0)
    {
        vec2 centred = uv - vec2(0.5);
        float angle = radians(vSdfPattern.z);
        vec2 rotated = vec2((centred.x * cos(angle)) - (centred.y * sin(angle)), (centred.x * sin(angle)) + (centred.y * cos(angle)));
        float mask = 0.0;
        if (vSdfPattern.x < 0.5)
        {
            mask = step(0.5, fract((rotated.x + rotated.y) * vSdfPattern.y));
        }
        else
        {
            vec2 grid = fract(rotated * vSdfPattern.y) - vec2(0.5);
            mask = 1.0 - step(0.25, length(grid));
        }
        paint *= (1.0 - (vSdfPattern.w * mask));
    }
    if (vSdfPattern.w > 0.0)
    {
        vec4 pattern = texture(uPattern, uv);
        vec4 _151 = paint;
        float _156 = paint.w;
        vec3 _161 = mix(_151.xyz, pattern.xyz * _156, vec3(vSdfPattern.w));
        paint.x = _161.x;
        paint.y = _161.y;
        paint.z = _161.z;
    }
    vec4 result = (paint * fill) * vSdfFillAlpha;
    if (vSdf.x > 0.0)
    {
        float outlineInner = 0.5 - vSdf.x;
        float outline = clamp(smoothstep(outlineInner - aa, outlineInner + aa, distanceValue) - fill, 0.0, 1.0);
        result += (vec4(vSdfOutlineColor.xyz * vSdfOutlineColor.w, vSdfOutlineColor.w) * outline);
        if (vSdf.w > 0.0)
        {
            float glowInner = (0.5 - vSdf.x) - vSdf.w;
            float glow = clamp((smoothstep(glowInner - aa, glowInner + aa, distanceValue) - fill) - outline, 0.0, 1.0);
            result += ((vec4(paint.xyz, paint.w) * glow) * 0.3499999940395355224609375);
        }
    }
    bool _255 = vSdf.y != 0.0;
    bool _262;
    if (!_255)
    {
        _262 = vSdf.z != 0.0;
    }
    else
    {
        _262 = _255;
    }
    if (_262)
    {
        float shadowDistance = texture(uTexture, uv - vSdf.yz).x;
        float shadow = smoothstep(0.5 - aa, 0.5 + aa, shadowDistance) * (1.0 - fill);
        result += (vec4(vSdfShadowColor.xyz * vSdfShadowColor.w, vSdfShadowColor.w) * shadow);
    }
    oColor = clamp(result, vec4(0.0), vec4(1.0));
}

)GLSL";
inline constexpr Source kSource{ backend::ShaderStage::Fragment, "sdf_pattern.frag", kEs, kDesktop, kBindings, kBindingCount };
} // namespace sdf_pattern_frag

/** @return true when @p a and @p b are the same text, without <cstring>. */
[[nodiscard]] inline bool compare(const char* a, const char* b) noexcept {
  size_t index = 0;
  while (a[index] != '\0' && a[index] == b[index]) ++index;
  return a[index] == b[index];
}

/** @return the source for @p name, or an invalid one when it is not embedded. */
[[nodiscard]] inline Source find(const char* name) noexcept {
  if (name == nullptr) return Source{};
  if (compare(name, "composite.frag")) return composite_frag::kSource;
  if (compare(name, "composite.vert")) return composite_vert::kSource;
  if (compare(name, "coverage.frag")) return coverage_frag::kSource;
  if (compare(name, "gradient.frag")) return gradient_frag::kSource;
  if (compare(name, "image.frag")) return image_frag::kSource;
  if (compare(name, "image_cubic.frag")) return image_cubic_frag::kSource;
  if (compare(name, "mask.frag")) return mask_frag::kSource;
  if (compare(name, "path.frag")) return path_frag::kSource;
  if (compare(name, "path.vert")) return path_vert::kSource;
  if (compare(name, "present.frag")) return present_frag::kSource;
  if (compare(name, "quad.frag")) return quad_frag::kSource;
  if (compare(name, "quad.vert")) return quad_vert::kSource;
  if (compare(name, "sdf.frag")) return sdf_frag::kSource;
  if (compare(name, "sdf_pattern.frag")) return sdf_pattern_frag::kSource;
  return Source{};
}

/** @return how many shaders are embedded, which a diagnostic reports. */
[[nodiscard]] inline size_t count() noexcept { return 14; }

} // namespace glsl
} // namespace shaders
} // namespace gpu
} // namespace graphics

#endif // CORAL_GPU_CANVAS_SHADERS_GLSL_H
