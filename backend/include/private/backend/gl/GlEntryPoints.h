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

#ifndef CORAL_BACKEND_GL_GL_ENTRY_POINTS_H
#define CORAL_BACKEND_GL_GL_ENTRY_POINTS_H

/**
 * @file GlEntryPoints.h
 *
 * Runtime loader for the OpenGL and EGL entry points the backend uses.
 *
 * The backend declares the GL types and constants it needs and resolves the functions through the
 * platform loader at run time, instead of including the platform's GL headers and linking against the
 * GL library. Three reasons:
 *
 *  1. `CoralGraphics` stays buildable on hosts that have no GL development headers installed, which is
 *     what lets the RHI's own test suite build and run anywhere.
 *  2. The same translation units serve both dialects -- desktop OpenGL 4.1 and OpenGL ES 3.0 -- so the
 *     dialect is a property of the loaded function table rather than a link-time choice.
 *  3. Desktop GL has no stable ABI above version 1.1: every newer function must be resolved through
 *     `wglGetProcAddress` / `glXGetProcAddress` regardless, so a loader is not optional there.
 *
 * The table below is the union of the two dialects. A function that exists in only one of them is
 * left null by the other dialect's loader, and the backend checks before calling it. Which entries
 * are *required* is a per-dialect decision made in `GLApi::load`.
 */

#include <backend/Types.h>
#include <utils/compiler.h>

#include <cstddef>
#include <cstdint>

// ------------------------------------------------------------------------------------------------
// GL types
// ------------------------------------------------------------------------------------------------
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLbyte = signed char;
using GLshort = short;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLushort = unsigned short;
using GLuint = unsigned int;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLclampd = double;
using GLfixed = int;
using GLintptr = ptrdiff_t;
using GLsizeiptr = ptrdiff_t;
using GLint64 = int64_t;
using GLuint64 = uint64_t;
using GLchar = char;
using GLsync = struct __GLsync*;

// ------------------------------------------------------------------------------------------------
// GL constants
//
// Only the ones the backend references. Values are identical between desktop GL and OpenGL ES unless
// a comment says otherwise, which is what lets one table serve both dialects.
// ------------------------------------------------------------------------------------------------

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NONE 0
#define GL_ZERO 0
#define GL_ONE 1

// Errors
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506

// Primitive types
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_PATCHES 0x000E

// Data types
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define GL_INT_2_10_10_10_REV 0x8D9F
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD

// Buffers
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020

// Shaders and programs
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPUTE_SHADER 0x91B9
#define GL_GEOMETRY_SHADER 0x8DD9
#define GL_TESS_CONTROL_SHADER 0x8E88
#define GL_TESS_EVALUATION_SHADER 0x8E87
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VALIDATE_STATUS 0x8B83
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_UNIFORM_BLOCK_BINDING 0x8A3F
#define GL_UNIFORM_BLOCK_DATA_SIZE 0x8A40
#define GL_UNIFORM_OFFSET 0x8A3B
#define GL_UNIFORM_ARRAY_STRIDE 0x8A3C
// `GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS` and `GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES` are the two this block needs, and
// they are easy to get wrong: `0x8A2E`/`0x8A2F` -- the values this file used to carry -- are
// `GL_MAX_COMBINED_UNIFORM_BLOCKS` and `GL_MAX_UNIFORM_BUFFER_BINDINGS`, so `glGetActiveUniformBlockiv` rejected both
// as `GL_INVALID_ENUM`, left the caller's count at zero, and the member-offset diagnostic below printed nothing at all
// while looking like it had run. The failure mode of a wrong enum is silence, which is why these are named here.
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS 0x8A42
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES 0x8A43
// The per-member size in basic machine units. There is no `GL_UNIFORM_DATA_SIZE`; `0x8A37` is `GL_UNIFORM_TYPE`, which
// is already defined below, and asking for it returns an enum where a byte count was expected.
#define GL_UNIFORM_MATRIX_STRIDE 0x8A3D
#define GL_UNIFORM_IS_ROW_MAJOR 0x8A3E
#define GL_UNIFORM_TYPE 0x8A37
#define GL_UNIFORM_SIZE 0x8A38
#define GL_UNIFORM_NAME_LENGTH 0x8A39
#define GL_UNIFORM_BLOCK_INDEX 0x8A3A

// Uniform types
#define GL_FLOAT_VEC2 0x8B50
#define GL_FLOAT_VEC3 0x8B51
#define GL_FLOAT_VEC4 0x8B52
#define GL_INT_VEC2 0x8B53
#define GL_INT_VEC3 0x8B54
#define GL_INT_VEC4 0x8B55
#define GL_BOOL 0x8B56
#define GL_BOOL_VEC2 0x8B57
#define GL_BOOL_VEC3 0x8B58
#define GL_BOOL_VEC4 0x8B59
#define GL_FLOAT_MAT2 0x8B5A
#define GL_FLOAT_MAT3 0x8B5B
#define GL_FLOAT_MAT4 0x8B5C
#define GL_SAMPLER_2D 0x8B5E
#define GL_SAMPLER_3D 0x8B5F
#define GL_SAMPLER_CUBE 0x8B60
#define GL_SAMPLER_2D_SHADOW 0x8B62
#define GL_SAMPLER_2D_ARRAY 0x8DC1
#define GL_SAMPLER_2D_ARRAY_SHADOW 0x8DC4
#define GL_SAMPLER_CUBE_SHADOW 0x8DC5
#define GL_INT_SAMPLER_2D 0x8DCA
#define GL_INT_SAMPLER_3D 0x8DCB
#define GL_INT_SAMPLER_CUBE 0x8DCC
#define GL_INT_SAMPLER_2D_ARRAY 0x8DCF
#define GL_UNSIGNED_INT_SAMPLER_2D 0x8DD2
#define GL_UNSIGNED_INT_SAMPLER_3D 0x8DD3
#define GL_UNSIGNED_INT_SAMPLER_CUBE 0x8DD4
#define GL_UNSIGNED_INT_SAMPLER_2D_ARRAY 0x8DD7
#define GL_SAMPLER_CUBE_MAP_ARRAY 0x900C
#define GL_FLOAT_MAT2x3 0x8B65
#define GL_FLOAT_MAT2x4 0x8B66
#define GL_FLOAT_MAT3x2 0x8B67
#define GL_FLOAT_MAT3x4 0x8B68
#define GL_FLOAT_MAT4x2 0x8B69
#define GL_FLOAT_MAT4x3 0x8B6A

// Textures
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_CUBE_MAP_ARRAY 0x9009
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X 0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y 0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y 0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z 0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E

// Texture formats
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_STENCIL 0x84F9
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RG 0x8227
#define GL_RED_INTEGER 0x8D94
#define GL_RG_INTEGER 0x8228
#define GL_RGB_INTEGER 0x8D98
#define GL_RGBA_INTEGER 0x8D99
#define GL_BGRA 0x80E1
#define GL_BGR 0x80E0
#define GL_ALPHA 0x1906
#define GL_LUMINANCE 0x1909
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_SRGB8 0x8C41
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_RGB10_A2 0x8059
#define GL_R11F_G11F_B10F 0x8C3A
#define GL_RGBA16F 0x881A
#define GL_RGB16F 0x881B
#define GL_RG16F 0x822F
#define GL_R16F 0x822D
#define GL_R32F 0x822E
#define GL_RG32F 0x8230
#define GL_RGB32F 0x8815
#define GL_RGBA32F 0x8814
#define GL_RGBA16I 0x8D88
#define GL_RGBA16UI 0x8D76
#define GL_RGBA8I 0x8D8E
#define GL_RGBA8UI 0x8D7C
#define GL_RGBA32I 0x8D82
#define GL_RGBA32UI 0x8D70
#define GL_R8 0x8229
#define GL_RG8 0x822B

// Normalised and integer formats. The signed-normalised and integer families are not colour
// renderable on the OpenGL ES 3.0 baseline, but they are valid texture and buffer formats, so the
// backend declares them for format translation while the capability table reports them accurately.
#define GL_R8_SNORM 0x8F94
#define GL_RG8_SNORM 0x8F95
#define GL_RGB8_SNORM 0x8F96
#define GL_RGBA8_SNORM 0x8F97
#define GL_RGB10_A2UI 0x906F
#define GL_R8I 0x8231
#define GL_R8UI 0x8232
#define GL_R16I 0x8233
#define GL_R16UI 0x8234
#define GL_R32I 0x8235
#define GL_R32UI 0x8236
#define GL_RG8I 0x8237
#define GL_RG8UI 0x8238
#define GL_RG16I 0x8239
#define GL_RG16UI 0x823A
#define GL_RG32I 0x823B
#define GL_RG32UI 0x823C
#define GL_RGB8I 0x8D8F
#define GL_RGB8UI 0x8D7D
#define GL_RGB16I 0x8D89
#define GL_RGB16UI 0x8D77
#define GL_RGB32I 0x8D83
#define GL_RGB32UI 0x8D71
#define GL_RGBA16 0x805B
#define GL_RGBA16_SNORM 0x8F9B
#define GL_RGB16 0x8054
#define GL_RG16 0x822C
#define GL_R16 0x822A
#define GL_RGB16_SNORM 0x8F9A
#define GL_RG16_SNORM 0x8F99
#define GL_R16_SNORM 0x8F98
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH32F_STENCIL8 0x8CAD
#define GL_COMPRESSED_R11_EAC 0x9270
#define GL_COMPRESSED_RG11_EAC 0x9272
#define GL_COMPRESSED_SIGNED_R11_EAC 0x9271
#define GL_COMPRESSED_SIGNED_RG11_EAC 0x9273
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#define GL_COMPRESSED_SRGB8_ETC2 0x9275
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#define GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC 0x9279
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR 0x93D0
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR 0x93B7
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR 0x93D7
// Desktop-only block compression. Reported as unsupported in the ES dialect, but the format
// translation table names them so a desktop context can use them.
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F

// Pixel store
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_IMAGE_HEIGHT 0x806E
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_IMAGE_HEIGHT 0x806D
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04

// Framebuffers
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_UNDEFINED 0x8219
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE 0x8D56
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_NONE_BIT 0

// Blending
#define GL_BLEND 0x0BE2
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_CONSTANT_COLOR 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_SRC1_COLOR 0x88F9
#define GL_ONE_MINUS_SRC1_COLOR 0x88FA
#define GL_SRC1_ALPHA 0x8589
#define GL_ONE_MINUS_SRC1_ALPHA 0x88FB
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_BLEND_COLOR 0x8005
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_COVERAGE 0x80A0

// Depth / stencil
#define GL_DEPTH_TEST 0x0B71
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_DEPTH_FUNC 0x0B74
#define GL_STENCIL_TEST 0x0B90
#define GL_STENCIL_FUNC 0x0B92
#define GL_STENCIL_REF 0x0B97
#define GL_STENCIL_VALUE_MASK 0x0B93
#define GL_STENCIL_WRITEMASK 0x0B98
#define GL_STENCIL_FAIL 0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL 0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS 0x0B96
#define GL_STENCIL_BACK_FUNC 0x8800
#define GL_STENCIL_BACK_REF 0x8CA3
#define GL_STENCIL_BACK_VALUE_MASK 0x8CA4
#define GL_STENCIL_BACK_WRITEMASK 0x8CA5
#define GL_STENCIL_BACK_FAIL 0x8801
#define GL_STENCIL_BACK_PASS_DEPTH_FAIL 0x8802
#define GL_STENCIL_BACK_PASS_DEPTH_PASS 0x8803
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_KEEP 0x1E00
#define GL_REPLACE 0x1E01
#define GL_INCR 0x1E02
#define GL_DECR 0x1E03
#define GL_INVERT 0x150A
#define GL_INCR_WRAP 0x8507
#define GL_DECR_WRAP 0x8508
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207

// Rasterisation
#define GL_CULL_FACE 0x0B44
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_POLYGON_OFFSET_FACTOR 0x8038
#define GL_POLYGON_OFFSET_UNITS 0x2A00
#define GL_SCISSOR_TEST 0x0C11
#define GL_VIEWPORT 0x0BA2
#define GL_DEPTH_CLAMP 0x864F
#define GL_MULTISAMPLE 0x809D
#define GL_SAMPLE_MASK 0x8E51
#define GL_SAMPLE_MASK_VALUE 0x8E52
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02
#define GL_POINT 0x1B00

// Strings and limits
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_NUM_EXTENSIONS 0x821D
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_3D_TEXTURE_SIZE 0x8073
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE 0x851C
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#define GL_MAX_ARRAY_TEXTURE_LAYERS 0x88FF
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#define GL_MAX_VARYING_VECTORS 0x8DFC
#define GL_MAX_UNIFORM_BUFFER_BINDINGS 0x8A2F
#define GL_MAX_UNIFORM_BLOCK_SIZE 0x8A30
#define GL_MAX_COLOR_ATTACHMENTS 0x8CDF
#define GL_MAX_DRAW_BUFFERS 0x8824
#define GL_MAX_SAMPLES 0x8D57
#define GL_MAX_SERVER_WAIT_TIMEOUT 0x9111
#define GL_MAX_COMPUTE_WORK_GROUP_COUNT 0x91BE
#define GL_MAX_COMPUTE_WORK_GROUP_SIZE 0x91BF
#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB
#define GL_MAX_ELEMENTS_INDICES 0x80E9
#define GL_MAX_ELEMENTS_VERTICES 0x80E8
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
#define GL_ALIASED_POINT_SIZE_RANGE 0x846D

// Queries
#define GL_ANY_SAMPLES_PASSED 0x8C2F
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#define GL_SAMPLES_PASSED 0x8914
#define GL_TIME_ELAPSED 0x88BF
#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_TIMESTAMP 0x8E28
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_CURRENT_QUERY 0x8865

// Sync
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_ALREADY_SIGNALED 0x911A
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull

// Debug output
#define GL_DEBUG_OUTPUT 0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#define GL_DEBUG_SEVERITY_MEDIUM 0x9147
#define GL_DEBUG_SEVERITY_LOW 0x9148
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B
#define GL_DEBUG_SOURCE_API 0x8246
#define GL_DEBUG_TYPE_ERROR 0x824C

#define GL_FRAMEBUFFER_SRGB 0x8DB9

// Memory barrier bits for glMemoryBarrier / glMemoryBarrierByRegion.
#define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 0x00000001
#define GL_ELEMENT_ARRAY_BARRIER_BIT 0x00000002
#define GL_UNIFORM_BARRIER_BIT 0x00000004
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#define GL_COMMAND_BARRIER_BIT 0x00000040
#define GL_PIXEL_BUFFER_BARRIER_BIT 0x00000080
#define GL_TEXTURE_UPDATE_BARRIER_BIT 0x00000100
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#define GL_FRAMEBUFFER_BARRIER_BIT 0x00000400
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#define GL_ALL_BARRIER_BITS 0xFFFFFFFF

// ------------------------------------------------------------------------------------------------
// Function pointer table
//
// The union of the two dialects. Functions that exist in only one of them are documented as such;
// the loader for the other dialect leaves them null and the backend checks before calling.
//
// Where the dialects have different spellings of the same operation (glDepthRange/glDepthRangef,
// glClearDepth/glClearDepthf) both entries are present rather than #ifdef'd, because that keeps the
// whole difference visible in one place: the row listing, and the two call sites that pick.
// ------------------------------------------------------------------------------------------------
#define CORAL_GL_FUNCTIONS(X)                                                                                          \
  X(void, glActiveTexture, (GLenum texture))                                                                           \
  X(void, glAttachShader, (GLuint program, GLuint shader))                                                             \
  X(void, glBeginQuery, (GLenum target, GLuint id))                                                                    \
  X(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar* name))                                    \
  X(void, glBindBuffer, (GLenum target, GLuint buffer))                                                                \
  X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer))                                              \
  X(void, glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))           \
  X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                                                      \
  X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer))                                                    \
  X(void, glBindSampler, (GLuint unit, GLuint sampler))                                                                \
  X(void, glBindTexture, (GLenum target, GLuint texture))                                                              \
  X(void, glBindVertexArray, (GLuint array))                                                                           \
  X(void, glBlendColor, (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha))                                     \
  X(void, glBlendEquationSeparate, (GLenum modeRGB, GLenum modeAlpha))                                                 \
  X(void, glBlendFuncSeparate, (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha))                       \
  X(void, glBlitFramebuffer,                                                                                           \
          (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,     \
                  GLbitfield mask, GLenum filter))                                                                     \
  X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                              \
  X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))                        \
  X(GLenum, glCheckFramebufferStatus, (GLenum target))                                                                 \
  X(void, glClear, (GLbitfield mask))                                                                                  \
  X(void, glClearBufferfv, (GLenum buffer, GLint drawbuffer, const GLfloat* value))                                    \
  X(void, glClearBufferfi, (GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil))                            \
  X(void, glClearColor, (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha))                                     \
  X(void, glClearDepth, (GLclampd depth))                                                                              \
  X(void, glClearDepthf, (GLclampf depth))                                                                             \
  X(void, glClearStencil, (GLint s))                                                                                   \
  X(void, glClearTexImage, (GLuint texture, GLint level, GLenum format, GLenum type, const void* data))                \
  X(void, glColorMask, (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha))                              \
  X(void, glCompileShader, (GLuint shader))                                                                            \
  X(GLuint, glCreateProgram, (void) )                                                                                  \
  X(GLuint, glCreateShader, (GLenum type))                                                                             \
  X(void, glCullFace, (GLenum mode))                                                                                   \
  X(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers))                                                         \
  X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                                               \
  X(void, glDeleteProgram, (GLuint program))                                                                           \
  X(void, glDeleteQueries, (GLsizei n, const GLuint* ids))                                                             \
  X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint* renderbuffers))                                             \
  X(void, glDeleteSamplers, (GLsizei count, const GLuint* samplers))                                                   \
  X(void, glDeleteShader, (GLuint shader))                                                                             \
  X(void, glDeleteSync, (GLsync sync))                                                                                 \
  X(void, glDeleteTextures, (GLsizei n, const GLuint* textures))                                                       \
  X(void, glEGLImageTargetTexture2DOES, (GLenum target, void* image))                                                   \
  X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                                     \
  X(void, glDepthFunc, (GLenum func))                                                                                  \
  X(void, glDepthMask, (GLboolean flag))                                                                               \
  X(void, glDepthRange, (GLclampd nearVal, GLclampd farVal))                                                           \
  X(void, glDepthRangef, (GLclampf nearVal, GLclampf farVal))                                                          \
  X(void, glDetachShader, (GLuint program, GLuint shader))                                                             \
  X(void, glDisable, (GLenum cap))                                                                                     \
  X(void, glDisableVertexAttribArray, (GLuint index))                                                                  \
  X(void, glDispatchCompute, (GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z))                          \
  X(void, glDispatchComputeIndirect, (GLintptr indirect))                                                              \
  X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                                                     \
  X(void, glDrawArraysIndirect, (GLenum mode, const void* indirect))                                                   \
  X(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instancecount))                     \
  X(void, glDrawBuffers, (GLsizei n, const GLenum* bufs))                                                              \
  X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))                              \
  X(void, glDrawElementsBaseVertex, (GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex))  \
  X(void, glDrawElementsIndirect, (GLenum mode, GLenum type, const void* indirect))                                    \
  X(void, glDrawElementsInstanced,                                                                                     \
          (GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount))                       \
  X(void, glDrawElementsInstancedBaseVertex,                                                                           \
          (GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount, GLint basevertex))     \
  X(void, glEnable, (GLenum cap))                                                                                      \
  X(void, glEnableVertexAttribArray, (GLuint index))                                                                   \
  X(void, glEndQuery, (GLenum target))                                                                                 \
  X(void, glFinish, (void) )                                                                                           \
  X(void, glFlush, (void) )                                                                                            \
  X(void, glFlushMappedBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length))                               \
  X(void, glFramebufferRenderbuffer,                                                                                   \
          (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer))                          \
  X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))   \
  X(void, glFrontFace, (GLenum mode))                                                                                  \
  X(void, glGenBuffers, (GLsizei n, GLuint * buffers))                                                                 \
  X(void, glGenFramebuffers, (GLsizei n, GLuint * framebuffers))                                                       \
  X(void, glGenQueries, (GLsizei n, GLuint * ids))                                                                     \
  X(void, glGenRenderbuffers, (GLsizei n, GLuint * renderbuffers))                                                     \
  X(void, glGenSamplers, (GLsizei count, GLuint * samplers))                                                           \
  X(void, glGenTextures, (GLsizei n, GLuint * textures))                                                               \
  X(void, glGenVertexArrays, (GLsizei n, GLuint * arrays))                                                             \
  X(void, glGenerateMipmap, (GLenum target))                                                                           \
  X(void, glGetActiveUniform,                                                                                          \
          (GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type,               \
                  GLchar * name))                                                                                      \
  X(void, glGetActiveUniformBlockiv, (GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint * params))         \
          X(void, glGetActiveUniformsiv, (GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname, GLint* params))     \
  X(GLint, glGetAttribLocation, (GLuint program, const GLchar* name))                                                  \
  X(GLenum, glGetError, (void) )                                                                                       \
  X(void, glGetFloatv, (GLenum pname, GLfloat * data))                                                                 \
  X(void, glGetIntegerv, (GLenum pname, GLint * data))                                                                 \
  X(void, glGetProgramInfoLog, (GLuint program, GLsizei maxLength, GLsizei * length, GLchar * infoLog))                \
  X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint * params))                                              \
  X(void, glGetQueryObjectuiv, (GLuint id, GLenum pname, GLuint * params))                                             \
  X(void, glGetQueryObjectui64v, (GLuint id, GLenum pname, GLuint64 * params))                                         \
  X(void, glGetShaderInfoLog, (GLuint shader, GLsizei maxLength, GLsizei * length, GLchar * infoLog))                  \
  X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint * params))                                                \
  X(const GLubyte*, glGetString, (GLenum name))                                                                        \
  X(const GLubyte*, glGetStringi, (GLenum name, GLuint index))                                                         \
  X(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar* uniformBlockName))                                  \
  X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))                                                 \
  X(void, glLinkProgram, (GLuint program))                                                                             \
  X(void*, glMapBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access))                   \
  X(void, glMemoryBarrier, (GLbitfield barriers))                                                                      \
  X(void, glMemoryBarrierByRegion, (GLbitfield barriers))                                                              \
  X(void, glObjectLabel, (GLenum identifier, GLuint name, GLsizei length, const GLchar* label))                        \
  X(void, glPixelStorei, (GLenum pname, GLint param))                                                                  \
  X(void, glPolygonMode, (GLenum face, GLenum mode))                                                                   \
  X(void, glPolygonOffset, (GLfloat factor, GLfloat units))                                                            \
  X(void, glPopDebugGroup, (void) )                                                                                    \
  X(void, glPushDebugGroup, (GLenum source, GLuint id, GLsizei length, const GLchar* message))                         \
  X(void, glReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels))   \
  X(void, glRenderbufferStorageMultisample,                                                                            \
          (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height))                      \
  X(void, glSampleMaski, (GLuint maskNumber, GLbitfield mask))                                                         \
  X(void, glSamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param))                                          \
  X(void, glSamplerParameterfv, (GLuint sampler, GLenum pname, const GLfloat* param))                                  \
  X(void, glSamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                                            \
  X(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height))                                                \
  X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))            \
  X(void, glStencilFuncSeparate, (GLenum face, GLenum func, GLint ref, GLuint mask))                                   \
  X(void, glStencilMaskSeparate, (GLenum face, GLuint mask))                                                           \
  X(void, glStencilOpSeparate, (GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass))                              \
  X(void, glTexImage2D,                                                                                                \
          (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,              \
                  GLenum format, GLenum type, const void* pixels))                                                     \
  X(void, glTexImage3D,                                                                                                \
          (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth,             \
                  GLint border, GLenum format, GLenum type, const void* pixels))                                       \
  X(void, glTexParameterf, (GLenum target, GLenum pname, GLfloat param))                                               \
  X(void, glTexParameterfv, (GLenum target, GLenum pname, const GLfloat* params))                                      \
  X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                                                 \
  X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height))       \
  X(void, glTexStorage3D,                                                                                              \
          (GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth))        \
  X(void, glTexSubImage2D,                                                                                             \
          (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format,     \
                  GLenum type, const void* pixels))                                                                    \
  X(void, glTexSubImage3D,                                                                                             \
          (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height,     \
                  GLsizei depth, GLenum format, GLenum type, const void* pixels))                                      \
  X(void, glUniform1f, (GLint location, GLfloat v0))                                                                   \
  X(void, glUniform1fv, (GLint location, GLsizei count, const GLfloat* value))                                         \
  X(void, glUniform1i, (GLint location, GLint v0))                                                                     \
  X(void, glUniform1iv, (GLint location, GLsizei count, const GLint* value))                                           \
  X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1))                                                       \
  X(void, glUniform2fv, (GLint location, GLsizei count, const GLfloat* value))                                         \
  X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                                           \
  X(void, glUniform3fv, (GLint location, GLsizei count, const GLfloat* value))                                         \
  X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))                               \
  X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat* value))                                         \
  X(void, glUniformBlockBinding, (GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding))               \
  X(void, glUniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value))              \
  X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value))              \
  X(GLboolean, glUnmapBuffer, (GLenum target))                                                                         \
  X(void, glUseProgram, (GLuint program))                                                                              \
  X(void, glVertexAttribDivisor, (GLuint index, GLuint divisor))                                                       \
  X(void, glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer))        \
  X(void, glVertexAttribPointer,                                                                                       \
          (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer))          \
  X(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height))                                               \
  X(GLsync, glFenceSync, (GLenum condition, GLbitfield flags))                                                         \
  X(GLenum, glClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                                       \
  X(void, glWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                                               \
  X(void, glBindImageTexture,                                                                                          \
          (GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format))

namespace backend {
namespace gl {

/**
 * The resolved GL function table.
 *
 * A single global instance, populated once at device creation. A member is null when the loaded
 * dialect does not provide it, so the backend tests before calling anything dialect-specific.
 */
struct GLApi {
#define CORAL_GL_DECLARE(ret, name, args) ret(*name) args = nullptr;
  CORAL_GL_FUNCTIONS(CORAL_GL_DECLARE)
#undef CORAL_GL_DECLARE

  /** The dialect this table was loaded for. */
  GlDialect dialect = GlDialect::ES;

  /** True once every entry point required by `dialect` has been resolved. */
  bool loaded = false;

  /**
   * The first required entry point that could not be resolved, or null when `loaded` is true.
   *
   * Kept so a failed device can say *which* function is missing: the platform layer returns a bare
   * `Unsupported`, and without the name the message only says "no context", which is what sent the
   * Windows GL investigation after the wrong cause.
   */
  const char* missingEntryPoint = nullptr;

  /**
   * Resolves the entry points for @p targetDialect.
   *
   * @param getProcAddress the platform's GL loader: `wglGetProcAddress`, `glXGetProcAddressARB` or
   *        `eglGetProcAddress`. Desktop GL has no stable ABI above version 1.1, so this is required
   *        there; on Android it may be null, in which case the platform's default loader is used.
   * @return true when every entry point the dialect requires was resolved.
   */
  bool load(GlDialect targetDialect, void* (*getProcAddress)(const char*) );

  /** @return a human readable name for a GL error code. */
  static const char* errorName(GLenum error) noexcept;
};

/** The process-wide resolved entry points. */
[[nodiscard]] GLApi& getGLApi() noexcept;

} // namespace gl
} // namespace backend

// ------------------------------------------------------------------------------------------------
// EGL entry points
//
// Used on Android, and as the headless path where no window system is available. Desktop context
// creation on Windows, Linux and macOS uses WGL, GLX and CGL respectively; see GlPlatform.h.
// ------------------------------------------------------------------------------------------------
typedef void* EGLNativeWindowType;
typedef void* EGLNativeDisplayType;
typedef void* EGLContextHandle;
typedef void* EGLSurfaceHandle;
typedef void* EGLConfigHandle;
typedef void* EGLDisplayHandle;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;
#define EGL_NATIVE_BUFFER_ANDROID 0x3140

/** The EGL subset the backend uses. Resolved on demand; all members may be null on non-EGL hosts. */
struct EGLApi {
  EGLDisplayHandle (*eglGetDisplay)(EGLNativeDisplayType) = nullptr;

  /**
   * EGL 1.5 / `EGL_EXT_platform_base`: the display constructor that names the platform.
   *
   * A Wayland display is not an X11 display and `eglGetDisplay` only guesses from the pointer. The
   * platform form is what lets the Linux backend create a Wayland display from a `wl_display*`, and
   * a surfaceless one for an offscreen fallback. Null on an EGL older than 1.5.
   */
  EGLDisplayHandle (*eglGetPlatformDisplay)(uint32_t platform, void* nativeDisplay, const intptr_t* attributes) = nullptr;
  EGLBoolean (*eglInitialize)(EGLDisplayHandle, EGLint*, EGLint*) = nullptr;
  EGLBoolean (*eglTerminate)(EGLDisplayHandle) = nullptr;
  EGLBoolean (*eglChooseConfig)(EGLDisplayHandle, const EGLint*, EGLConfigHandle*, EGLint, EGLint*) = nullptr;
  EGLSurfaceHandle (
          *eglCreateWindowSurface)(EGLDisplayHandle, EGLConfigHandle, EGLNativeWindowType, const EGLint*) = nullptr;
  EGLSurfaceHandle (*eglCreatePbufferSurface)(EGLDisplayHandle, EGLConfigHandle, const EGLint*) = nullptr;
  EGLContextHandle (*eglCreateContext)(EGLDisplayHandle, EGLConfigHandle, EGLContextHandle, const EGLint*) = nullptr;
  EGLBoolean (*eglMakeCurrent)(EGLDisplayHandle, EGLSurfaceHandle, EGLSurfaceHandle, EGLContextHandle) = nullptr;
  EGLBoolean (*eglSwapBuffers)(EGLDisplayHandle, EGLSurfaceHandle) = nullptr;
  EGLBoolean (*eglSwapInterval)(EGLDisplayHandle, EGLint) = nullptr;
  EGLBoolean (*eglDestroySurface)(EGLDisplayHandle, EGLSurfaceHandle) = nullptr;
  EGLBoolean (*eglDestroyContext)(EGLDisplayHandle, EGLContextHandle) = nullptr;
  EGLint (*eglGetError)(void) = nullptr;
  EGLBoolean (*eglQuerySurface)(EGLDisplayHandle, EGLSurfaceHandle, EGLint, EGLint*) = nullptr;
  /** Android image import for AHardwareBuffer-backed OES textures. */
  void* (*eglGetNativeClientBufferANDROID)(const void*) = nullptr;
  void* (*eglCreateImageKHR)(EGLDisplayHandle, EGLContextHandle, EGLenum, const void*, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroyImageKHR)(EGLDisplayHandle, void*) = nullptr;

  /** Selects desktop OpenGL or OpenGL ES for the contexts this thread creates. */
  EGLBoolean (*eglBindAPI)(EGLint api) = nullptr;

  /** True once loadEGL() has resolved the entry points needed to create a context. */
  bool loaded = false;

  /**
   * Resolves the EGL entry points.
   *
   * @param getProcAddress a loader supplied by the windowing layer, or null to use the platform's
   *        default (dlsym against the already-mapped libEGL on Android).
   * @return true when the entry points needed to create a context were resolved.
   */
  static bool loadEGL(void* (*getProcAddress)(const char*) );
};

/** The process-wide resolved EGL entry points. */
[[nodiscard]] EGLApi& getEGLApi() noexcept;

// EGL constants used above
#define EGL_NONE 0x3038
#define EGL_TRUE 1
#define EGL_FALSE 0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#define EGL_OPENGL_BIT 0x0008
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SAMPLES 0x3031
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x0001
#define EGL_COLOR_BUFFER_TYPE 0x303F
#define EGL_RGB_BUFFER 0x308E
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_ACCESS 0x3002
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_CONTEXT 0x3006
#define EGL_BAD_CURRENT_SURFACE 0x3007
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_PIXMAP 0x300A
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_BAD_PARAMETER 0x300C
#define EGL_BAD_SURFACE 0x300D
#define EGL_CONTEXT_LOST 0x300E
#define EGL_NO_CONTEXT ((EGLContextHandle) 0)
#define EGL_NO_DISPLAY ((EGLDisplayHandle) 0)
#define EGL_NO_SURFACE ((EGLSurfaceHandle) 0)

/* Client APIs for `eglBindAPI`. */
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_API 0x30A2

/* Platform selectors for `eglGetPlatformDisplay`. */
#define EGL_PLATFORM_X11_KHR 0x31D5
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

namespace backend {
/** @return a human readable name for an EGL error code. */
[[nodiscard]] const char* eglErrorName(int32_t error) noexcept;

/**
 * Resolves a GL symbol the way the EGL platform does: `eglGetProcAddress` first, then the platform
 * library.
 *
 * Exposed because the EGL platform file needs the same resolution order the GL entry point loader
 * uses, and duplicating it would risk the two drifting apart.
 *
 * @return the symbol, or null when neither source provides it.
 */
[[nodiscard]] void* resolveEGLSymbol(const char* name) noexcept;
} // namespace backend

#endif // CORAL_BACKEND_GL_GL_ENTRY_POINTS_H



