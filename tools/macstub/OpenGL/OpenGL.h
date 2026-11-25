/* A minimal OpenGL/OpenGL.h, covering only what this translation unit calls. */
#ifndef CORAL_STUB_OPENGL_H
#define CORAL_STUB_OPENGL_H

#include <OpenGL/CGLTypes.h>

/*
 * The OpenGL scalar types the CGL entry points take.
 *
 * On macOS these come from `<OpenGL/gl.h>`, which the real `OpenGL.h` includes. Only `GLint` is used here -- the
 * version query writes through one -- but `GLenum` and `GLuint` are declared too, because a stub that declared only
 * what happens to be used today would stop compiling the moment the file used one more, which is the opposite of what
 * a check is for.
 */
#ifndef CORAL_STUB_GL_TYPES_DEFINED
#define CORAL_STUB_GL_TYPES_DEFINED
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned char GLubyte;
#endif

/*
 * Pixel-format attributes.
 *
 * The numbers are Apple's own and are not sequential: `kCGLPFADoubleBuffer` is 5, below the block that starts at 8, and
 * the profile attributes are in a later block again. Getting one of these wrong would request a different pixel format
 * rather than fail to compile, so every value here was read from `OpenGL.framework/Headers/CGLTypes.h` as it appears in
 * the 10.13 and 10.15 SDKs -- which agree -- rather than inferred from the order of the members.
 */
typedef enum {
  kCGLPFAAllRenderers = 1,
  kCGLPFATripleBuffer = 3,
  kCGLPFADoubleBuffer = 5,
  kCGLPFAColorSize = 8,
  kCGLPFAAlphaSize = 11,
  kCGLPFADepthSize = 12,
  kCGLPFAStencilSize = 13,
  kCGLPFAAccelerated = 73,
  kCGLPFAOpenGLProfile = 99,
} CGLPixelFormatAttributeValues;

/*
 * The OpenGL profiles a pixel format can request.
 *
 * `kCGLOGLPVersion_GL4_Core` is what this file asks for, because the shader compiler emits GLSL 4.10 core. The other
 * two are the legacy and 3.2-core profiles, declared so that the enumeration is recognisable as one.
 *
 * These are `CGLPixelFormatAttribute` values in the framework rather than a separate enum -- they appear in an
 * attribute list as the value following `kCGLPFAOpenGLProfile` -- which is why they are spelled as plain constants
 * here and cast at the call site.
 */
enum {
  kCGLOGLPVersion_Legacy = 0x1000,
  kCGLOGLPVersion_3_2_Core = 0x3200,
  kCGLOGLPVersion_GL3_Core = 0x3200,
  kCGLOGLPVersion_GL4_Core = 0x4100,
};

/*
 * Context parameters, read back with `CGLGetParameter`.
 *
 * The values are the framework's, and the enumeration deliberately stops where Apple's does. An earlier version of this
 * stub ended with `kCGLCPMajorVersion = 206` and `kCGLCPMinorVersion = 207`, which were **invented**: no Apple SDK
 * defines either, and `kCGLCPMPSwapsInFlight` is the last real member. Supplying names the platform does not have is
 * the one thing a stub must never do -- it turns "this code cannot compile here" into a pass, which is exactly what
 * happened, and it hid a file that had never built on macOS.
 *
 * If a future caller needs a parameter that is missing, it is missing from Apple's header too, and the answer is to
 * check what the framework actually exposes rather than to add it here.
 */
typedef enum {
  kCGLCPSwapRectangle = 200,
  kCGLCPSwapInterval = 222,
  kCGLCPDispatchTableSize = 224,
  kCGLCPClientStorage = 226,
  kCGLCPSurfaceOrder = 235,
  kCGLCPSurfaceOpacity = 236,
  kCGLCPSurfaceBackingSize = 304,
  kCGLCPSurfaceSurfaceVolatile = 306,
  kCGLCPReclaimResources = 308,
  kCGLCPCurrentRendererID = 309,
  kCGLCPGPUVertexProcessing = 310,
  kCGLCPGPUFragmentProcessing = 311,
  kCGLCPHasDrawable = 314,
  kCGLCPMPSwapsInFlight = 315,
} CGLContextParameter;

#ifdef __cplusplus
extern "C" {
#endif

/** Picks a pixel format matching @p attribs, which is terminated by a zero attribute. */
CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute* attribs, CGLPixelFormatObj* pix, GLint* npix);

void CGLDestroyPixelFormat(CGLPixelFormatObj pix);

/** Creates a context for @p pix, sharing with @p share if it is not null. */
CGLError CGLCreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj* ctx);

void CGLDestroyContext(CGLContextObj ctx);

/** Makes @p ctx the calling thread's current context, or clears it when @p ctx is null. */
CGLError CGLSetCurrentContext(CGLContextObj ctx);

/** @return the context's pixel format, or null when it has none. */
CGLPixelFormatObj CGLGetPixelFormat(CGLContextObj ctx);

/** Reads one of the context's parameters into @p value. */
CGLError CGLGetParameter(CGLContextObj ctx, CGLContextParameter pname, GLint* params);

/** Sets one of the context's parameters from @p params; the swap interval is the one this tree fixes. */
CGLError CGLSetParameter(CGLContextObj ctx, CGLContextParameter pname, const GLint* params);

/** Presents the context's attached drawable. */
CGLError CGLFlushDrawable(CGLContextObj ctx);

#ifdef __cplusplus
}
#endif

#endif /* CORAL_STUB_OPENGL_H */
