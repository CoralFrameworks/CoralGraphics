/* A minimal GL/glx.h, covering only what this translation unit calls. */
#ifndef CORAL_STUB_GL_GLX_H
#define CORAL_STUB_GL_GLX_H

#include <X11/Xlib.h>

/**
 * GLX's handle types.
 *
 * `GLXContext` and `GLXFBConfig` are pointers in the real header -- a caller stores one and passes it back -- so they
 * are pointers here. That matters: this file casts between `void*` and `GLXContext` in both directions, and a typedef of
 * an integer would make those casts ill-formed here while being correct on Linux.
 *
 * `GLXDrawable` is the exception: it is an `XID`, an integer, which is why the file goes through `uintptr_t` to move a
 * `void*` surface into and out of it.
 */
typedef struct __GLXcontextRec* GLXContext;
typedef struct __GLXFBConfigRec* GLXFBConfig;
typedef unsigned long GLXDrawable;

/** The OpenGL byte type, which GLX's extension lookup takes a name as. GL defines it too; a second definition is an
 *  error on MSVC, so the guard keeps the two stubs from colliding if a translation unit includes both. */
#ifndef CORAL_STUB_GL_TYPES_DEFINED
#define CORAL_STUB_GL_TYPES_DEFINED
typedef unsigned char GLubyte;
#endif

/* The attributes this file asks a framebuffer config for. */
#define GLX_X_RENDERABLE 0x8012
#define GLX_DRAWABLE_TYPE 0x8010

/*
 * GLX's boolean spellings, and its attribute-list terminator, which the file spells `None`.
 *
 * All three arrive on Linux through the real `<GL/glx.h>`, which includes Xlib's `X.h`: that is where `True`, `False`
 * and `None` come from, and it is why they survive the file's own `#undef None` -- the `#undef` runs before `glx.h` is
 * included. Reproduced here for the same reason they exist there. `X11/Xlib.h` deliberately does *not* define them,
 * because anything it defined would be undone by that `#undef`.
 */
#ifndef True
#define True 1
#endif
#ifndef False
#define False 0
#endif
#ifndef None
#define None 0L
#endif

#define GLX_RENDER_TYPE 0x8011
#define GLX_X_VISUAL_TYPE 0x22
#define GLX_RGBA_BIT 0x00000001
#define GLX_WINDOW_BIT 0x00000001
#define GLX_PBUFFER_BIT 0x00000004
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_DOUBLEBUFFER 5
#define GLX_PBUFFER_WIDTH 0x8041
#define GLX_PBUFFER_HEIGHT 0x8040

/**
 * The generic extension entry point, as GLX defines it.
 *
 * The real header has exactly this: `typedef void (*__GLXextFuncPtr)(void)`, and `glXGetProcAddressARB` returns one.
 * Declaring it as returning `void*` instead would be looser than the real prototype and would let this file's
 * conversions look portable when they are only accepted by one compiler.
 */
typedef void (*__GLXextFuncPtr)(void);

#ifdef __cplusplus
extern "C" {
#endif

GLXFBConfig* glXChooseFBConfig(Display* display, int screen, const int* attributes, int* configCount);

GLXContext glXCreateContext(Display* display, GLXFBConfig config, GLXContext share, Bool direct);

GLXDrawable glXCreatePbuffer(Display* display, GLXFBConfig config, const int* attributes);

void glXDestroyContext(Display* display, GLXContext context);

void glXDestroyPbuffer(Display* display, GLXDrawable pbuffer);

Bool glXMakeCurrent(Display* display, GLXDrawable drawable, GLXContext context);

void glXSwapBuffers(Display* display, GLXDrawable drawable);

/**
 * Resolves an extension entry point by name, or null when the driver does not have it.
 *
 * `ARB` is the spelling this file uses; the 1.4 core entry point is `glXGetProcAddress`, which is the same function
 * under a different name. Only the one the file calls is declared.
 */
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte* name);

#ifdef __cplusplus
}
#endif

#endif /* CORAL_STUB_GL_GLX_H */
